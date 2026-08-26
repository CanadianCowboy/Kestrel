#include "runtime/tensorrtbackend.h"

namespace kestrel::runtime {

BackendKind TensorRTBackend::kind() const noexcept {
    return BackendKind::TensorRT;
}

RuntimeStatus TensorRTBackend::status() const {
    return m_status;
}

bool TensorRTBackend::loadModel(const std::string&, std::string& error) {
    error = "TensorRT-LLM is not linked in this build";
    return false;
}

void TensorRTBackend::generate(const GenerationRequest&, TokenCallback,
                               CompletionCallback onComplete) {
    onComplete(false, "TensorRT-LLM is not linked in this build");
}

void TensorRTBackend::cancel() {}

} // namespace kestrel::runtime
