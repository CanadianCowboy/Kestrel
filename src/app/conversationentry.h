#pragma once

#include <QDateTime>
#include <QString>

#include <vector>

#include "core/conversation.h"

namespace kestrel::app {

// Delivery state of a single message as shown in the UI. Terminal states are
// kept with the conversation so partial or failed responses stay visible and
// recoverable, mirroring the preservation rules of core::VoiceSession.
enum class MessageStatus {
    Streaming,
    Complete,
    Stopped,
    Failed,
};

struct MessageExtra {
    MessageStatus status = MessageStatus::Complete;
    QString note; // e.g. an error detail for Failed messages
};

// One conversation as managed by the application layer: the portable core
// domain object plus UI-facing delivery metadata (parallel to the message
// list; ConversationEntry owners must mutate both together).
struct ConversationEntry {
    int id = 0;
    core::Conversation conversation;
    std::vector<MessageExtra> extras;
    QDateTime updatedAt;
};

} // namespace kestrel::app
