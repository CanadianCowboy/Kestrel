#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace kestrel::core {

enum class MessageRole {
    User,
    Assistant,
    System,
    Tool,
};

struct Message {
    MessageRole role;
    std::string content;
};

class Conversation {
public:
    explicit Conversation(std::string title = "New conversation");

    [[nodiscard]] const std::string& title() const noexcept;
    void setTitle(std::string title);

    void addMessage(MessageRole role, std::string content);
    void clear();
    [[nodiscard]] const std::vector<Message>& messages() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::string m_title;
    std::vector<Message> m_messages;
};

} // namespace kestrel::core
