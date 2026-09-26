#include "runtime/mockbackend.h"

#include "runtime/chatformat.h"

#include <algorithm>
#include <sstream>
#include <string>

namespace kestrel::runtime {

namespace {

constexpr std::size_t kContextLimit = 4096;

} // namespace

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
        // Advisory only. The app measures real throughput from delivered
        // tokens; reporting a made-up rate here would be a number nothing
        // produced.
        0.0,
        m_contextUsed,
        kContextLimit,
    };
}

bool MockBackend::loadModel(const std::string&, std::string&) {
    return true;
}

std::size_t MockBackend::countTokens(std::string_view text) const {
    // The mock has no vocabulary, so the shared approximation is the honest
    // answer rather than pretending to a tokenizer it does not have.
    return ModelBackend::countTokens(text);
}

void MockBackend::resetContextUsage() {
    m_contextUsed = 0;
}

void MockBackend::setSystemPrompt(std::string_view text) {
    // Stored, not cached: there is no KV cache here to keep it in, so the
    // prefix is charged on every turn exactly as a backend with no prefix
    // support would be. That makes the mock a fair preview of the cost.
    m_systemPrompt = text;
}

void MockBackend::clearSharedPrefix() {
    m_systemPrompt.clear();
}

void MockBackend::generate(const GenerationRequest& request,
                           TokenCallback onToken,
                           CompletionCallback onComplete) {
    m_cancelled.store(false, std::memory_order_release);

    // Echo the most recent user turn, which is what the caller actually asked.
    std::string question;
    for (const ChatMessage& message : request.messages) {
        if (message.role == Role::User && !message.content.empty()) {
            question = message.content;
        }
    }

    const std::string response =
        "This is a local preview response. Kestrel's runtime boundary is ready for "
        "llama.cpp or TensorRT, and the focused workspace can be refined without a "
        "model installed yet.\n\nYou said: " + question;

    std::istringstream words(response);
    std::string word;
    while (words >> word && !m_cancelled.load(std::memory_order_acquire)) {
        onToken(word + " ");
    }

    // Context grows by the shared prefix, the prompt and whatever this turn
    // produced, capped at the window so the UI cannot show an impossible fill
    // level.
    m_contextUsed = std::min(kContextLimit,
                             m_contextUsed + countTokens(m_systemPrompt) +
                                 countTokens(renderPlainChat(request.messages, true)) +
                                 countTokens(response));

    if (m_cancelled.load(std::memory_order_acquire)) {
        onComplete(false, "Generation stopped");
    } else {
        onComplete(true, {});
    }
}

void MockBackend::cancel() {
    m_cancelled.store(true, std::memory_order_release);
}

} // namespace kestrel::runtime
