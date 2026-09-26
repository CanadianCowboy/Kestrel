#include "runtime/cudadevice.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace kestrel::runtime {

namespace {

constexpr double kBytesPerGiB = 1073741824.0;

std::string formatWithOneDecimal(double value, const char* suffix) {
    std::array<char, 64> buffer{};
    const int written = std::snprintf(buffer.data(), buffer.size(), "%.1f %s", value, suffix);
    return std::string(buffer.data(), static_cast<std::size_t>(written < 0 ? 0 : written));
}

} // namespace

std::string CudaDeviceInfo::computeCapability() const {
    if (computeMajor <= 0) {
        return {};
    }
    return std::to_string(computeMajor) + "." + std::to_string(computeMinor);
}

std::string CudaDeviceInfo::memorySummary() const {
    return formatBytes(totalMemoryBytes);
}

double CudaDeviceInfo::gigabytesPerSecond() const {
    if (memoryClockRateKhz <= 0 || memoryBusWidthBits <= 0) {
        return 0.0;
    }
    // double data rate: bytes per second = 2 * clock * (bus width / 8)
    const double bytesPerSecond =
        2.0 * static_cast<double>(memoryClockRateKhz) * 1000.0
        * (static_cast<double>(memoryBusWidthBits) / 8.0);
    return bytesPerSecond / 1e9;
}

bool CudaRuntimeInfo::driverSufficient() const noexcept {
    return driverVersion > 0 && runtimeVersion > 0 && driverVersion >= runtimeVersion;
}

bool CudaProbe::hasDevice() const noexcept {
    return selectedDeviceIndex >= 0
        && static_cast<std::size_t>(selectedDeviceIndex) < devices.size();
}

const CudaDeviceInfo* CudaProbe::selectedDevice() const noexcept {
    if (!hasDevice()) {
        return nullptr;
    }
    return &devices[static_cast<std::size_t>(selectedDeviceIndex)];
}

std::string CudaProbe::summary() const {
    if (!runtime.available) {
        return "CUDA unavailable: " + runtime.detail;
    }
    const CudaDeviceInfo* device = selectedDevice();
    if (device == nullptr) {
        return "CUDA " + formatCudaVersion(runtime.runtimeVersion) + " available, no device selected";
    }
    const std::string prefix = device->index == 0 ? std::string{} : "GPU " + std::to_string(device->index) + ": ";
    return prefix + device->name + " (sm_" + device->computeCapability() + ", "
         + device->memorySummary() + ", CUDA " + formatCudaVersion(runtime.runtimeVersion) + ")";
}

std::string formatCudaVersion(int version) {
    if (version <= 0) {
        return {};
    }
    return std::to_string(version / 1000) + "." + std::to_string((version % 1000) / 10);
}

std::string formatBytes(std::size_t bytes) {
    const double asGiB = static_cast<double>(bytes) / kBytesPerGiB;
    if (asGiB >= 1.0) {
        return formatWithOneDecimal(asGiB, "GiB");
    }
    return formatWithOneDecimal(static_cast<double>(bytes) / 1048576.0, "MiB");
}

bool isTensorCoreCapable(int computeMajor, int computeMinor) noexcept {
    static_cast<void>(computeMinor);
    return computeMajor >= 7;
}

int resolveSelectedDeviceIndex(const std::vector<CudaDeviceInfo>& devices,
                               int preferredDeviceIndex) noexcept {
    if (devices.empty()) {
        return -1;
    }
    if (preferredDeviceIndex >= 0
        && static_cast<std::size_t>(preferredDeviceIndex) < devices.size()) {
        return preferredDeviceIndex;
    }
    return 0;
}

std::string describeDevice(const CudaDeviceInfo& device) {
    std::string description = device.name + " (sm_" + device.computeCapability() + ", "
        + device.memorySummary() + ", " + std::to_string(device.multiprocessorCount) + " SMs";
    if (isTensorCoreCapable(device.computeMajor, device.computeMinor)) {
        description += ", tensor cores";
    }
    const double bandwidth = device.gigabytesPerSecond();
    if (bandwidth > 0.0) {
        std::array<char, 64> buffer{};
        const int written = std::snprintf(buffer.data(), buffer.size(), ", %.0f GB/s", bandwidth);
        description += std::string(buffer.data(), static_cast<std::size_t>(written < 0 ? 0 : written));
    }
    description += ")";
    return description;
}

} // namespace kestrel::runtime
