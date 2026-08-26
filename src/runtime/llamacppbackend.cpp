#include "runtime/llamacppbackend.h"

namespace kestrel::runtime {

BackendKind LlamaCppBackend::kind() const noexcept {
    return BackendKind::LlamaCpp;
}

RuntimeStatus LlamaCppBackend::status() const {
    return m_status;
}

bool LlamaCppBackend::loadModel(const std::string&, std::string& error) {
    error = "llama.cpp is not linked in this build";
    return false;
}

void LlamaCppBackend::generate(const GenerationRequest&, TokenCallback,
                               CompletionCallback onComplete) {
    onComplete(false, "llama.cpp is not linked in this build");
}

void LlamaCppBackend::cancel() {}

} // namespace kestrel::runtime
