#include "runtime/backendregistry.h"

#include <memory>
#include <utility>

#include "runtime/llamacppbackend.h"
#include "runtime/mockbackend.h"
#include "runtime/tensorrtbackend.h"

namespace kestrel::runtime {

namespace {

std::unique_ptr<ModelBackend> makeBackend(BackendKind kind) {
    switch (kind) {
    case BackendKind::Mock: return std::make_unique<MockBackend>();
    case BackendKind::LlamaCpp: return std::make_unique<LlamaCppBackend>();
    case BackendKind::TensorRT: return std::make_unique<TensorRTBackend>();
    }
    return std::make_unique<MockBackend>();
}

// Preference order for an unspecified request: real accelerators first, mock last.
constexpr BackendKind kPreferenceOrder[] = {
    BackendKind::TensorRT,
    BackendKind::LlamaCpp,
    BackendKind::Mock,
};

void append(std::vector<RuntimeDiagnostic>& out, std::string label, std::string value, bool ok) {
    out.push_back({std::move(label), std::move(value), ok});
}

} // namespace

std::string_view toString(BackendKind kind) noexcept {
    switch (kind) {
    case BackendKind::Mock: return "mock";
    case BackendKind::LlamaCpp: return "llamacpp";
    case BackendKind::TensorRT: return "tensorrt";
    }
    return "unknown";
}

std::vector<RuntimeDiagnostic> runtimeDiagnostics(const CudaProbe& probe) {
    std::vector<RuntimeDiagnostic> out;

    // Build the CUDA toolkit into the binary?
#ifdef KESTREL_HAS_CUDA
    append(out, "CUDA toolkit", "compiled in", true);
#else
    append(out, "CUDA toolkit", "not compiled in (configure with -DKESTREL_ENABLE_CUDA=ON)", false);
#endif

#ifdef KESTREL_HAS_TENSORRT
    append(out, "TensorRT", "compiled in", true);
#else
    append(out, "TensorRT", "not compiled in (configure with -DKESTREL_ENABLE_TENSORRT=ON)", false);
#endif

    append(out, "CUDA driver",
           probe.runtime.driverVersion > 0
               ? "CUDA " + formatCudaVersion(probe.runtime.driverVersion)
               : std::string("unavailable"),
           probe.runtime.driverVersion > 0);

    if (probe.runtime.available) {
        const CudaDeviceInfo* device = probe.selectedDevice();
        if (device != nullptr) {
            append(out, "GPU", describeDevice(*device), true);
            append(out, "Tensor cores",
                   isTensorCoreCapable(device->computeMajor, device->computeMinor)
                       ? "available (sm_" + device->computeCapability() + ")"
                       : "not available (sm_" + device->computeCapability() + ")",
                   isTensorCoreCapable(device->computeMajor, device->computeMinor));
        }
        if (probe.devices.size() > 1) {
            append(out, "Devices", std::to_string(probe.devices.size()) + " visible", true);
        }
    } else {
        append(out, "GPU", probe.runtime.detail, false);
    }

    for (const BackendKind kind : kPreferenceOrder) {
        const auto backend = makeBackend(kind);
        const RuntimeStatus status = backend->status();
        append(out, std::string("Backend: ") + std::string(toString(kind)),
               status.detail.empty() ? status.backendName : status.detail,
               status.available);
    }

    return out;
}

std::unique_ptr<ModelBackend> selectBackend(BackendKind preferred) {
    if (preferred != BackendKind::Mock) {
        auto requested = makeBackend(preferred);
        if (requested->status().available) {
            return requested;
        }
    }

    for (const BackendKind kind : kPreferenceOrder) {
        if (kind == BackendKind::Mock) {
            continue;
        }
        auto candidate = makeBackend(kind);
        if (candidate->status().available) {
            return candidate;
        }
    }

    return makeBackend(BackendKind::Mock);
}

} // namespace kestrel::runtime
