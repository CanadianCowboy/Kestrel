#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::core {

enum class MessageRole {
    User,
    Assistant,
    System,
    // Local activity recorded for the user, such as an idle-loop tool. It is
    // never sent back to the model.
    Tool,
    // The result of a tool the model called from chat. The model reads it on its
    // next pass, so it is part of the conversation.
    ToolResult,
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

    // Streams additional content into the most recent message. Returns false
    // when the conversation is empty.
    bool appendToLastMessage(std::string_view text);

    // Removes the most recent message, e.g. when regenerating a response.
    // Returns false when the conversation is empty.
    bool removeLastMessage();

    void clear();
    [[nodiscard]] const std::vector<Message>& messages() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::string m_title;
    std::vector<Message> m_messages;
};

} // namespace kestrel::core
