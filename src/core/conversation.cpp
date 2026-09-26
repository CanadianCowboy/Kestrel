#include "core/conversation.h"

#include <utility>

namespace kestrel::core {

Conversation::Conversation(std::string title)
    : m_title(std::move(title)) {}

const std::string& Conversation::title() const noexcept {
    return m_title;
}

void Conversation::setTitle(std::string title) {
    m_title = std::move(title);
}

void Conversation::addMessage(MessageRole role, std::string content) {
    m_messages.push_back({role, std::move(content)});
}

bool Conversation::appendToLastMessage(std::string_view text) {
    if (m_messages.empty()) {
        return false;
    }
    m_messages.back().content.append(text);
    return true;
}

bool Conversation::removeLastMessage() {
    if (m_messages.empty()) {
        return false;
    }
    m_messages.pop_back();
    return true;
}

void Conversation::clear() {
    m_messages.clear();
}

const std::vector<Message>& Conversation::messages() const noexcept {
    return m_messages;
}

std::size_t Conversation::size() const noexcept {
    return m_messages.size();
}

} // namespace kestrel::core
