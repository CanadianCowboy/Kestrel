#pragma once

#include <string>
#include <string_view>

#include "runtime/cudadevice.h"

namespace kestrel::runtime {

// Metadata written beside a serialized engine by the offline build tooling.
//
// A TensorRT engine is not portable. It is bound to the TensorRT version that
// produced it, to a CUDA version, and to the GPU architecture it was built for.
// Deserializing a mismatched engine fails deep inside TensorRT with an opaque
// message that does not tell the user what to do. Kestrel therefore keeps a
// small sidecar record next to the engine so the desktop app can reject the
// artifact up front with an explanation the user can act on.
//
// The record is plain `key=value` text rather than JSON: it is written and read
// by offline tooling, it must stay trivially inspectable and diffable, and it
// avoids adding a serialization dependency to a project that vendors none.
struct EngineBuildRecord {
    // TensorRT's own NV_TENSORRT_VERSION packing: major*1000 + minor*100 + patch.
    // 10400 renders as "10.4", 8601 renders as "8.6.1".
    int tensorrtVersion = 0;
    // The CUDA runtime's packing: major*1000 + minor*10 + patch.
    // 13040 renders as "13.4", 11030 renders as "11.3".
    int cudaVersion = 0;
    int computeMajor = 0;
    int computeMinor = 0;
    std::string gpuName;
    std::string builtBy;
};

enum class EngineCompatibility {
    // Sidecar present and consistent with this machine.
    Compatible,
    // Loadable, but something differs that has bitten users before.
    CompatibleWithWarning,
    // Must not be loaded here; the report says why.
    Incompatible,
    // No sidecar record, so nothing can be asserted either way.
    Unknown,
};

struct EngineCompatibilityReport {
    EngineCompatibility verdict = EngineCompatibility::Unknown;
    bool hasRecord = false;
    EngineBuildRecord record;
    // One line describing the outcome, safe to show in the UI.
    std::string summary;
    // What the user should do. Empty when the verdict is Compatible.
    std::string remedy;

    [[nodiscard]] bool loadable() const noexcept;
    [[nodiscard]] static std::string_view toString(EngineCompatibility verdict) noexcept;
};

// Suffix appended to the engine file name, e.g. "model.plan.kestrel-engine".
[[nodiscard]] std::string_view engineSidecarSuffix() noexcept;
[[nodiscard]] std::string engineSidecarPath(const std::string& enginePath);

// 10400 -> "10.4", 8601 -> "8.6.1". Returns an empty string for 0 or negative input.
[[nodiscard]] std::string formatTensorRTVersion(int version);

// -1, 0 or 1, following the usual comparison convention.
[[nodiscard]] int compareVersions(int lhs, int rhs) noexcept;

// Compares an engine's build record against what this machine actually has.
//
// `tensorrtVersion` is the version of the runtime that would deserialize the
// engine; pass 0 when no TensorRT runtime is present, which downgrades a
// version mismatch to a warning instead of a hard failure because there is
// nothing to compare against yet. A missing sidecar yields Unknown, which is
// loadable: absence of metadata is not evidence of a problem.
[[nodiscard]] EngineCompatibilityReport checkEngineCompatibility(const EngineBuildRecord& record,
                                                                const CudaProbe& probe,
                                                                int tensorrtVersion = 0);

// Reads the sidecar for `enginePath`. Returns false and fills `error` when the
// record is absent or malformed.
[[nodiscard]] bool readEngineBuildRecord(const std::string& enginePath,
                                         EngineBuildRecord& record,
                                         std::string& error);

// Writes the sidecar for `enginePath`. Intended for the offline engine-build
// workflow, not for the desktop process, which must never modify engines.
[[nodiscard]] bool writeEngineBuildRecord(const std::string& enginePath,
                                          const EngineBuildRecord& record,
                                          std::string& error);

} // namespace kestrel::runtime
