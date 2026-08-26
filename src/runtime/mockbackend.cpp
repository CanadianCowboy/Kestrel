#include "runtime/mockbackend.h"

#include <sstream>
#include <string>

namespace kestrel::runtime {

BackendKind MockBackend::kind() const noexcept {
    return BackendKind::Mock;
}

RuntimeStatus MockBackend::status() const {
    return {
        true,
        true,
        "Mock runtime",
        "Kestrel demo model",
        "UI preview mode",
        0.0,
        0,
        4096,
    };
}

bool MockBackend::loadModel(const std::string&, std::string&) {
    return true;
}

void MockBackend::generate(const GenerationRequest& request,
                           TokenCallback onToken,
                           CompletionCallback onComplete) {
    m_cancelled = false;
    const std::string response =
        "This is a local preview response. Kestrel's runtime boundary is ready for "
        "llama.cpp or TensorRT, and the focused workspace can be refined without a "
        "model installed yet.\n\nYour prompt was: " + request.prompt;

    std::istringstream words(response);
    std::string word;
    while (words >> word && !m_cancelled) {
        onToken(word + " ");
    }

    if (m_cancelled) {
        onComplete(false, "Generation stopped");
    } else {
        onComplete(true, {});
    }
}

void MockBackend::cancel() {
    m_cancelled = true;
}

} // namespace kestrel::runtime
