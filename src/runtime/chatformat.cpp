#include "runtime/chatformat.h"

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
    } else if (!out.empty() && !messages.empty()
               && messages.back().role == Role::Assistant && out.back() == '\n') {
        out.pop_back(); // Continue the partial assistant text without a turn boundary.
    }
    return out;
}

} // namespace kestrel::runtime
