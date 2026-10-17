#include "runtime/chatformat.h"

#include <algorithm>
#include <utility>

namespace kestrel::runtime {

const char* toString(Role role) noexcept {
    switch (role) {
        case Role::System:
            return "system";
        case Role::User:
            return "user";
        case Role::Assistant:
            return "assistant";
        case Role::Tool:
            return "tool";
    }
    return "user";
}

namespace {

// True for the roles that belong in a conversation proper. System messages are
// handled separately because they are the cached prefix, not a turn.
bool isTurn(Role role) noexcept {
    return role != Role::System;
}

void appendTurn(std::string& out, const ChatMessage& message) {
    // A label, not a delimiter, so the plain format reads as a transcript a
    // base model can complete. Empty content is skipped rather than rendered as
    // a bare label, which is what an in-flight assistant turn looks like.
    if (message.content.empty()) {
        return;
    }
    out += toString(message.role);
    out += ": ";
    out += message.content;
    out += "\n";
}

} // namespace

std::string renderPlainPrefix(const std::vector<ChatMessage>& messages) {
    std::string prefix;
    for (const ChatMessage& message : messages) {
        if (message.role != Role::System || message.content.empty()) {
            continue;
        }
        prefix += message.content;
        prefix += "\n\n";
    }
    return prefix;
}

std::string renderPlainChat(const std::vector<ChatMessage>& messages, bool addAssistantCue) {
    // Starts with exactly renderPlainPrefix's output, so a prefix decoded from
    // that remains valid when this full rendering is decoded on top of it.
    std::string out = renderPlainPrefix(messages);
    for (const ChatMessage& message : messages) {
        if (!isTurn(message.role)) {
            continue;
        }
        appendTurn(out, message);
    }
    if (addAssistantCue) {
        out += "assistant:";
    } else if (!messages.empty()
               && messages.back().role == Role::Assistant) {
        if (messages.back().content.empty()) {
            out += "assistant:";
        } else if (out.back() == '\n') {
            out.pop_back(); // Continue the partial assistant text without a turn boundary.
        }
    }
    return out;
}

std::string PlainTurnFilter::append(std::string_view text) {
    if (m_stopped) { return {}; }
    m_pending.append(text);
    constexpr std::string_view markers[] = {"\nuser:", "\nUser:", "\nassistant:", "\nAssistant:",
                                            "\nsystem:", "\nSystem:", "\ntool:"};
    std::size_t stop = std::string::npos;
    for (const auto marker : markers) { stop = std::min(stop, m_pending.find(marker)); }
    if (stop != std::string::npos) {
        std::string safe = m_pending.substr(0, stop);
        m_pending.clear();
        m_stopped = true;
        return safe;
    }
    std::size_t held = 0;
    for (const auto marker : markers) {
        for (std::size_t length = 1; length < marker.size() && length <= m_pending.size(); ++length) {
            if (m_pending.ends_with(marker.substr(0, length))) { held = std::max(held, length); }
        }
    }
    const std::size_t ready = m_pending.size() - held;
    std::string safe = m_pending.substr(0, ready);
    m_pending.erase(0, ready);
    return safe;
}

std::string PlainTurnFilter::finish() {
    return m_stopped ? std::string{} : std::exchange(m_pending, {});
}

} // namespace kestrel::runtime
