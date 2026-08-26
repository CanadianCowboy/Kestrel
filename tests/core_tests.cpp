#include "core/conversation.h"
#include "runtime/mockbackend.h"

#include <cassert>
#include <string>

int main() {
    using namespace kestrel;

    core::Conversation conversation;
    assert(conversation.size() == 0);
    conversation.addMessage(core::MessageRole::User, "Hello");
    conversation.addMessage(core::MessageRole::Assistant, "Hi there");
    assert(conversation.size() == 2);
    assert(conversation.messages().front().content == "Hello");
    conversation.clear();
    assert(conversation.size() == 0);

    runtime::MockBackend backend;
    assert(backend.status().available);
    std::string streamed;
    bool completed = false;
    backend.generate({"test", 0.7F, 32},
                     [&streamed](std::string_view token) { streamed += token; },
                     [&completed](bool success, std::string_view) { completed = success; });
    assert(completed);
    assert(streamed.find("local preview response") != std::string::npos);

    return 0;
}
