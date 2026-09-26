#include "runtime/cudadevice.h"

#include <string>

// Selected by CMake when the CUDA Toolkit is absent. It keeps the portable
// surface complete so every consumer compiles and links on machines with no
// toolkit and no driver, and it reports an actionable reason instead of
// pretending the GPU is simply missing.

namespace kestrel::runtime {

std::string cudaErrorName(int code) noexcept {
    if (code == 0) {
        return "cudaSuccess";
    }
    return "cudaError" + std::to_string(code);
}

CudaProbe probeCuda(int preferredDeviceIndex) noexcept {
    static_cast<void>(preferredDeviceIndex);

    CudaProbe probe;
    probe.runtime.available = false;
    probe.runtime.errorName = "cudaErrorNotProbed";
    probe.runtime.detail =
        "This build was compiled without the CUDA Toolkit, so device discovery is "
        "compiled out. Configure with -DKESTREL_ENABLE_CUDA=ON and a CUDA Toolkit on "
        "PATH to enable GPU support.";
    return probe;
}

} // namespace kestrel::runtime
