#include "runtime/tensorrtbackend.h"

#include "core/pathtext.h"

#include <filesystem>
#include <utility>

#ifdef KESTREL_HAS_TENSORRT
#include <NvInferVersion.h>
#endif

namespace kestrel::runtime {

namespace {

std::string modelDisplayName(const std::string& modelPath) {
    const std::filesystem::path path(modelPath);
    // core::pathText rather than filename().string(): a model file under a user
    // name the ANSI code page cannot spell throws from string() on Windows.
    return path.has_filename() ? core::pathText(path.filename()) : modelPath;
}

} // namespace

TensorRTBackend::TensorRTBackend()
    : m_probe(probeCuda()) {
    m_status.backendName = "TensorRT";
    m_status.modelName = "No engine loaded";
    refreshDevices();
}

TensorRTBackend::~TensorRTBackend() = default;

void TensorRTBackend::refreshDevices() {
    m_probe = probeCuda();
    m_status.available = false;
    m_status.contextLimit = 0;

    if (!m_probe.runtime.available) {
        m_status.detail = m_probe.runtime.detail;
        return;
    }

    const CudaDeviceInfo* device = m_probe.selectedDevice();
    if (device == nullptr) {
        m_status.detail = "CUDA is available but no device was selected.";
        return;
    }

    m_status.detail = "Ready for engine validation on " + m_probe.summary() + ".";
}

BackendKind TensorRTBackend::kind() const noexcept {
    return BackendKind::TensorRT;
}

int TensorRTBackend::embeddedTensorRTVersion() const noexcept {
#ifdef KESTREL_HAS_TENSORRT
    return NV_TENSORRT_MAJOR * 1000 + NV_TENSORRT_MINOR * 100 + NV_TENSORRT_PATCH;
#else
    return 0;
#endif
}

RuntimeStatus TensorRTBackend::status() const {
    RuntimeStatus status = m_status;
    const CudaDeviceInfo* device = m_probe.selectedDevice();
    if (device != nullptr) {
        status.modelName = m_modelPath.empty() ? std::string("No engine loaded") : m_modelDisplayName;
    }
    return status;
}

std::string TensorRTBackend::unavailableDetail() const {
    if (!m_probe.runtime.available) {
        return "TensorRT is unavailable because CUDA device discovery failed: "
            + m_probe.runtime.detail;
    }
    if (embeddedTensorRTVersion() > 0) {
        return "TensorRT " + formatTensorRTVersion(embeddedTensorRTVersion())
            + " is linked, but engine deserialization is not implemented yet.";
    }
    return "TensorRT is not linked in this build. Configure with -DKESTREL_ENABLE_TENSORRT=ON "
           "and point CMAKE_PREFIX_PATH at a TensorRT SDK to enable it.";
}

bool TensorRTBackend::loadModel(const std::string& modelPath, std::string& error) {
    m_cancelled.store(false);
    m_modelPath.clear();
    m_modelDisplayName.clear();
    m_status.modelLoaded = false;
    m_compatibility = EngineCompatibilityReport{};

    if (modelPath.empty()) {
        error = "No engine path was provided.";
        m_status.detail = error;
        return false;
    }

    std::error_code fileError;
    const std::filesystem::path path(modelPath);
    if (!std::filesystem::exists(path, fileError)) {
        error = "Engine file not found: " + modelPath;
        m_status.detail = error;
        return false;
    }
    if (!std::filesystem::is_regular_file(path, fileError)) {
        error = "Engine path is not a file: " + modelPath;
        m_status.detail = error;
        return false;
    }
    const auto engineBytes = std::filesystem::file_size(path, fileError);
    if (fileError) {
        error = "Could not read the engine file " + modelPath + ": " + fileError.message();
        m_status.detail = error;
        return false;
    }
    if (engineBytes == 0) {
        error = "Engine file is empty: " + modelPath;
        m_status.detail = error;
        return false;
    }

    m_modelPath = modelPath;
    m_modelDisplayName = modelDisplayName(modelPath);

    // A missing record is not evidence of a problem, so it downgrades to a
    // warning rather than blocking the load.
    EngineBuildRecord record;
    std::string recordError;
    if (readEngineBuildRecord(modelPath, record, recordError)) {
        m_compatibility = checkEngineCompatibility(record, m_probe, embeddedTensorRTVersion());
    } else {
        m_compatibility.verdict = EngineCompatibility::Unknown;
        m_compatibility.hasRecord = false;
        m_compatibility.summary = "No engine build record found, so this engine could not be "
                                  "verified against the current GPU and toolkit versions.";
        m_compatibility.remedy = "Re-run the offline engine-build step so Kestrel can write "
                                 + std::string(engineSidecarSuffix()) + " beside the engine.";
    }

    if (!m_compatibility.loadable()) {
        error = m_compatibility.summary + " " + m_compatibility.remedy;
        m_status.detail = error;
        m_modelPath.clear();
        m_modelDisplayName.clear();
        return false;
    }

    error.clear();
    m_status.modelLoaded = true;
    m_status.modelName = m_modelDisplayName;
    m_status.detail = m_compatibility.summary;
    if (!m_compatibility.remedy.empty()) {
        m_status.detail += " " + m_compatibility.remedy;
    }
    if (embeddedTensorRTVersion() <= 0) {
        m_status.detail += " The engine passed validation, but this build cannot deserialize it.";
    }
    return true;
}

void TensorRTBackend::generate(const GenerationRequest& request,
                               TokenCallback onToken,
                               CompletionCallback onComplete) {
    static_cast<void>(onToken);
    static_cast<void>(request);

    m_cancelled.store(false);

    if (m_cancelled.load()) {
        onComplete(false, "Generation cancelled before it started");
        return;
    }
    if (m_modelPath.empty()) {
        onComplete(false, "No TensorRT engine is loaded. Call loadModel() first.");
        return;
    }
    onComplete(false, unavailableDetail());
}

void TensorRTBackend::cancel() {
    m_cancelled.store(true);
}

} // namespace kestrel::runtime
