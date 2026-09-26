#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "runtime/cudadevice.h"
#include "runtime/engineartifact.h"
#include "runtime/modelbackend.h"

namespace kestrel::runtime {

// Adapter boundary for native TensorRT inference.
//
// TensorRT is not vendored, and Kestrel deliberately does not pretend to run
// without it. What this adapter does own today is everything that can be
// verified without the SDK: it probes the GPU, validates the engine artifact
// and its build record against this machine, and refuses to load an engine that
// is known to be incompatible *before* TensorRT can produce an opaque
// deserialization failure.
//
// The deserialization and execution steps are the documented seam. Once
// -DKESTREL_ENABLE_TENSORRT=ON and the SDK are present, `KESTREL_HAS_TENSORRT`
// gates the real path; until then the backend reports a precise reason it is
// unavailable rather than a generic "not linked" message.
class TensorRTBackend final : public ModelBackend {
public:
    TensorRTBackend();
    ~TensorRTBackend() override;

    // Re-reads CUDA device state. Call after the user changes the selected
    // device, before loading an engine.
    void refreshDevices();

    [[nodiscard]] BackendKind kind() const noexcept override;
    [[nodiscard]] RuntimeStatus status() const override;

    // Validates `modelPath` and its sidecar build record against the current
    // device. Returns false with an actionable message when the engine is
    // missing or known to be incompatible. A validated engine is not yet
    // deserialized; see status().detail for the remaining step.
    bool loadModel(const std::string& modelPath, std::string& error) override;

    void generate(const GenerationRequest& request,
                  TokenCallback onToken,
                  CompletionCallback onComplete) override;

    // Sets the cancellation flag. Safe to call while a generation is in
    // flight, and safe to call when nothing is running.
    void cancel() override;

    [[nodiscard]] const CudaProbe& probe() const noexcept { return m_probe; }
    [[nodiscard]] const EngineCompatibilityReport& lastCompatibility() const noexcept {
        return m_compatibility;
    }
    [[nodiscard]] int embeddedTensorRTVersion() const noexcept;

private:
    [[nodiscard]] std::string unavailableDetail() const;

    CudaProbe m_probe;
    EngineCompatibilityReport m_compatibility;
    RuntimeStatus m_status;
    std::string m_modelPath;
    std::string m_modelDisplayName;
    std::atomic_bool m_cancelled{false};
};

} // namespace kestrel::runtime
