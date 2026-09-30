#pragma once

#include "runtime/modelbackend.h"

#include <string>
#include <vector>

namespace kestrel::runtime {

// Portable chat rendering, with no knowledge of any particular model family.
//
// A model that ships a chat template should be asked how it wants to be talked
// to, because getting that wrong is the single most visible way to make a
// capable model behave like a broken one: an instruct model fed a raw
// "User:/Assistant:" transcript often answers worse than the same model in its
// own format, and stops using the turn structure it was trained on.
//
// A base model has no such format, so these renderers give it the plainest
// structure available. Both paths are exercised by the tests, because "works
// when a template exists" is not the same claim as "works at all".

// Renders the leading, turn-independent part of a conversation: the system
// messages and nothing else. This is what a backend can decode once and keep
// resident, so every later turn only pays for its own tokens.
//
// Only meaningful because the full rendering of a conversation begins with
// this exact text. A template that does not satisfy that is detected at
// render time rather than assumed, because assuming it would silently
// invalidate the cache on every turn.
std::string renderPlainPrefix(const std::vector<ChatMessage>& messages);

// Renders the whole conversation. `addAssistantCue` appends the marker that
// tells the model its turn has come, which is what makes the next token the
// beginning of a reply rather than more user text.
std::string renderPlainChat(const std::vector<ChatMessage>& messages, bool addAssistantCue);

[[nodiscard]] const char* toString(Role role) noexcept;

} // namespace kestrel::runtime
