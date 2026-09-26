#include "runtime/cudadevice.h"

#include <cuda_runtime.h>

#include <string>
#include <utility>

// Selected by CMake when the CUDA Toolkit is found. This is the only
// translation unit in the project allowed to include a CUDA header; the rest of
// the codebase sees the portable structs in cudadevice.h.

namespace kestrel::runtime {

namespace {

std::string formatUuid(const cudaUUID_t& uuid) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string text = "GPU-";
    text.reserve(40);
    for (std::size_t i = 0; i < sizeof(uuid.bytes); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            text += '-';
        }
        const auto byte = static_cast<unsigned char>(uuid.bytes[i]);
        text += kHex[byte >> 4];
        text += kHex[byte & 0x0FU];
    }
    return text;
}

// Clock rates, bus width and cache size are device *attributes*, not struct
// fields: CUDA 13 removed them from cudaDeviceProp. A failed attribute query is
// not fatal, it just leaves that capability unreported.
int readAttribute(int device, cudaDeviceAttr attribute) noexcept {
    int value = 0;
    if (cudaDeviceGetAttribute(&value, attribute, device) != cudaSuccess) {
        cudaGetLastError();
        return 0;
    }
    return value;
}

std::string describeError(const char* operation, cudaError_t error) {
    const std::string name = cudaErrorName(error);
    const std::string reason = cudaGetErrorString(error);

    if (error == cudaErrorInsufficientDriver) {
        return std::string(operation) + " failed (" + name
            + "): the installed NVIDIA driver is older than the CUDA runtime this build "
              "was compiled against. Update the driver, or rebuild against an older toolkit.";
    }
    if (error == cudaErrorNoDevice) {
        return std::string(operation) + " failed (" + name
            + "): no CUDA-capable device is visible to this process.";
    }
    if (error == cudaErrorInitializationError) {
        return std::string(operation) + " failed (" + name
            + "): the CUDA driver could not be initialised. Check that the driver is "
              "installed and that no other process is holding a conflicting context.";
    }
    if (error == cudaErrorSystemDriverMismatch) {
        return std::string(operation) + " failed (" + name
            + "): the installed driver does not match this CUDA toolkit. Install a driver "
              "supported by the toolkit version, or rebuild against a matching toolkit.";
    }
    if (error == cudaErrorStubLibrary) {
        return std::string(operation) + " failed (" + name
            + "): the CUDA runtime is only partially installed; cudart could not be loaded.";
    }
    return std::string(operation) + " failed (" + name + "): " + reason;
}

void queryDriverVersion(CudaRuntimeInfo& info) {
    int driverVersion = 0;
    if (cudaDriverGetVersion(&driverVersion) == cudaSuccess) {
        info.driverVersion = driverVersion;
    }
    int runtimeVersion = 0;
    if (cudaRuntimeGetVersion(&runtimeVersion) == cudaSuccess) {
        info.runtimeVersion = runtimeVersion;
    }
}

} // namespace

std::string cudaErrorName(int code) noexcept {
    const char* name = cudaGetErrorName(static_cast<cudaError_t>(code));
    if (name == nullptr || *name == '\0') {
        return "cudaError" + std::to_string(code);
    }
    return name;
}

CudaProbe probeCuda(int preferredDeviceIndex) noexcept {
    CudaProbe probe;
    CudaRuntimeInfo& info = probe.runtime;

    queryDriverVersion(info);

    int count = 0;
    const cudaError_t countError = cudaGetDeviceCount(&count);
    if (countError != cudaSuccess) {
        info.available = false;
        info.errorCode = static_cast<int>(countError);
        info.errorName = cudaErrorName(static_cast<int>(countError));
        info.detail = describeError("cudaGetDeviceCount", countError);
        return probe;
    }

    info.deviceCount = count;
    if (count <= 0) {
        info.available = false;
        info.detail = "The CUDA runtime initialised but reported no devices. Check that a "
                      "supported NVIDIA GPU is installed and visible to this process.";
        return probe;
    }

    probe.devices.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        cudaDeviceProp props{};
        const cudaError_t propError = cudaGetDeviceProperties(&props, i);
        if (propError != cudaSuccess) {
            // One unreadable device must not hide the readable ones.
            continue;
        }
        CudaDeviceInfo device;
        device.index = i;
        device.name = props.name;
        device.uuid = formatUuid(props.uuid);
        device.computeMajor = props.major;
        device.computeMinor = props.minor;
        device.totalMemoryBytes = props.totalGlobalMem;
        device.sharedMemoryPerBlockBytes = props.sharedMemPerBlock;
        device.l2CacheBytes = static_cast<std::size_t>(
            readAttribute(i, cudaDevAttrL2CacheSize));
        device.multiprocessorCount = props.multiProcessorCount;
        device.clockRateKhz = readAttribute(i, cudaDevAttrClockRate);
        device.memoryClockRateKhz = readAttribute(i, cudaDevAttrMemoryClockRate);
        device.memoryBusWidthBits = readAttribute(i, cudaDevAttrGlobalMemoryBusWidth);
        device.warpSize = props.warpSize;
        device.maxThreadsPerBlock = props.maxThreadsPerBlock;
        device.maxThreadsPerMultiprocessor = props.maxThreadsPerMultiProcessor;
        device.asyncEngineCount = props.asyncEngineCount;
        device.integrated = props.integrated != 0;
        device.canMapHostMemory = props.canMapHostMemory != 0;
        probe.devices.push_back(std::move(device));
    }

    if (probe.devices.empty()) {
        info.available = false;
        info.detail = "CUDA reported " + std::to_string(count)
            + " device(s) but none could be described; the driver may be too old for this "
              "CUDA toolkit.";
        return probe;
    }

    // Enumeration indices must stay aligned with CUDA device indices, because
    // callers may hand a probe selection back to cudaSetDevice().
    probe.selectedDeviceIndex = resolveSelectedDeviceIndex(probe.devices, preferredDeviceIndex);
    const CudaDeviceInfo* selected = probe.selectedDevice();
    if (selected == nullptr) {
        info.available = false;
        info.detail = "The requested CUDA device index is out of range.";
        return probe;
    }

    info.available = true;
    info.detail = selected->name + " (sm_" + selected->computeCapability() + ", "
        + selected->memorySummary() + ")";
    if (probe.devices.size() > 1) {
        info.detail += " — " + std::to_string(probe.devices.size() - 1)
            + " more device(s) available";
    }
    if (!info.driverSufficient()) {
        info.detail += "; driver " + formatCudaVersion(info.driverVersion)
            + " is older than toolkit " + formatCudaVersion(info.runtimeVersion)
            + ", so some toolkits will refuse to load engines built for this GPU";
    }
    return probe;
}

} // namespace kestrel::runtime
