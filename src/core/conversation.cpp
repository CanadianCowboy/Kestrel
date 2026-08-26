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
