#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::runtime {

// Portable description of a CUDA-capable device.
//
// This header deliberately includes no CUDA or Windows headers. The portable
// core must build and run on machines with no toolkit, no driver, and no GPU,
// and `app`/`ui` must be able to read device facts through the same header on
// every configuration. `cudadiscovery_cuda.cpp` fills these fields in behind a
// translation-unit swap that CMake selects, so callers never include a vendor
// header to learn what hardware is present.
struct CudaDeviceInfo {
    int index = -1;
    std::string name;
    std::string uuid;
    int computeMajor = 0;
    int computeMinor = 0;
    std::size_t totalMemoryBytes = 0;
    std::size_t sharedMemoryPerBlockBytes = 0;
    std::size_t l2CacheBytes = 0;
    int multiprocessorCount = 0;
    int clockRateKhz = 0;
    int memoryClockRateKhz = 0;
    int memoryBusWidthBits = 0;
    int warpSize = 0;
    int maxThreadsPerBlock = 0;
    int maxThreadsPerMultiprocessor = 0;
    int asyncEngineCount = 0;
    bool integrated = false;
    bool canMapHostMemory = false;

    // "8.9"
    [[nodiscard]] std::string computeCapability() const;
    // "8.0 GiB"
    [[nodiscard]] std::string memorySummary() const;
    // Theoretical peak transfer rate implied by the reported memory clock and
    // bus width, assuming double data rate. A capability hint, not a benchmark.
    [[nodiscard]] double gigabytesPerSecond() const;
};

// What the CUDA runtime reported about itself on this machine.
struct CudaRuntimeInfo {
    // True only when the runtime initialized and at least one device answered.
    bool available = false;
    int driverVersion = 0;
    int runtimeVersion = 0;
    int deviceCount = 0;
    // Raw CUDA error code, 0 when none. Uninterpreted on purpose: it is passed
    // straight to cudaGetErrorName() in the CUDA build and simply stays 0 in
    // the no-toolkit build.
    int errorCode = 0;
    // Symbolic error name from the runtime, e.g. "cudaErrorNoDevice".
    std::string errorName;
    // Actionable sentence explaining availability, phrased for a user.
    std::string detail;

    [[nodiscard]] bool driverSufficient() const noexcept;
};

// Result of a device-discovery pass. Safe to copy; owns all of its strings.
struct CudaProbe {
    CudaRuntimeInfo runtime;
    std::vector<CudaDeviceInfo> devices;
    int selectedDeviceIndex = -1;

    [[nodiscard]] bool hasDevice() const noexcept;
    [[nodiscard]] const CudaDeviceInfo* selectedDevice() const noexcept;
    // One-line summary for logs, the UI status bar, and error messages.
    [[nodiscard]] std::string summary() const;
};

// 13040 -> "13.4". Returns an empty string for 0 or negative input.
[[nodiscard]] std::string formatCudaVersion(int version);

// Binary-prefix byte formatting: 8589934592 -> "8.0 GiB".
[[nodiscard]] std::string formatBytes(std::size_t bytes);

// Volta (7.0) and newer expose tensor cores. Returns false for unknown or
// pre-Volta architectures.
[[nodiscard]] bool isTensorCoreCapable(int computeMajor, int computeMinor) noexcept;

// Enumerates CUDA devices and reports runtime/driver health.
//
// Never throws and never blocks on model work: this runs once at startup and on
// demand from the UI. `preferredDeviceIndex` selects a device when several are
// present; negative means "use the first usable device". When no device is
// available the result still explains why, so callers can surface an
// actionable message instead of a bare "no GPU".
[[nodiscard]] CudaProbe probeCuda(int preferredDeviceIndex = -1) noexcept;

// Name of a CUDA error code, using the runtime's own symbolic names. Returns a
// "cudaError<n>" placeholder for values the runtime does not recognise, and
// "cudaErrorNotProbed" outside a CUDA-enabled build.
[[nodiscard]] std::string cudaErrorName(int code) noexcept;

// Resolves the search hint for `probeCuda`: honours an explicit valid index,
// otherwise the first device, otherwise -1. Exposed for tests.
[[nodiscard]] int resolveSelectedDeviceIndex(const std::vector<CudaDeviceInfo>& devices,
                                             int preferredDeviceIndex) noexcept;

// Renders a device as a single human-readable line.
[[nodiscard]] std::string describeDevice(const CudaDeviceInfo& device);

} // namespace kestrel::runtime
