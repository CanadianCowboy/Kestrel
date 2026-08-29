#include "core/voicesession.h"

#include <algorithm>
#include <utility>

namespace kestrel::core {

namespace {

bool isActiveState(ResponseState state) noexcept {
    return state == ResponseState::Generating || state == ResponseState::Speaking;
}

bool isWhitespace(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

bool isSentenceTerminator(char c) noexcept {
    return c == '.' || c == '!' || c == '?' || c == '\n';
}

} // namespace

const char* toString(ResponseState state) noexcept {
    switch (state) {
    case ResponseState::Queued: return "queued";
    case ResponseState::Generating: return "generating";
    case ResponseState::Speaking: return "speaking";
    case ResponseState::Paused: return "paused";
    case ResponseState::Interrupted: return "interrupted";
    case ResponseState::Completed: return "completed";
    case ResponseState::Cancelled: return "cancelled";
    case ResponseState::Failed: return "failed";
    }
    return "unknown";
}

const char* toString(InterruptionIntent intent) noexcept {
    switch (intent) {
    case InterruptionIntent::Pause: return "pause";
    case InterruptionIntent::Correction: return "correction";
    case InterruptionIntent::Question: return "question";
    case InterruptionIntent::Replacement: return "replacement";
    case InterruptionIntent::Resume: return "resume";
    }
    return "unknown";
}

std::size_t sentenceStartBefore(std::string_view text, std::size_t offset) noexcept {
    const std::size_t limit = std::min(offset, text.size());
    std::size_t start = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        if (!isSentenceTerminator(text[i])) {
            continue;
        }
        std::size_t next = i + 1;
        while (next < limit && isWhitespace(text[next])) {
            ++next;
        }
        start = next;
    }
    return start;
}

std::string_view VoiceResponse::spokenText() const noexcept {
    return std::string_view(m_generatedText).substr(0, m_spokenOffset);
}

std::string_view VoiceResponse::unspokenText() const noexcept {
    return std::string_view(m_generatedText).substr(m_spokenOffset);
}

bool VoiceResponse::isTerminal() const noexcept {
    return m_state == ResponseState::Completed
        || m_state == ResponseState::Cancelled
        || m_state == ResponseState::Failed;
}

ResponseId VoiceSession::queueResponse(std::string runtimeContext) {
    VoiceResponse response;
    response.m_id = m_nextResponseId++;
    response.m_runtimeContext = std::move(runtimeContext);
    const ResponseId id = response.m_id;
    m_responses.push_back(std::move(response));
    pushEvent(VoiceEventKind::ResponseQueued, id);
    return id;
}

GenerationId VoiceSession::beginGeneration(ResponseId id) {
    VoiceResponse* response = findMutable(id);
    if (response == nullptr || response->m_state != ResponseState::Queued || otherResponseActive(id)) {
        return kInvalidGenerationId;
    }
    response->m_state = ResponseState::Generating;
    response->m_expectedGeneration = nextGeneration();
    pushEvent(VoiceEventKind::GenerationStarted, id);
    return response->m_expectedGeneration;
}

bool VoiceSession::appendText(ResponseId id, GenerationId generation, std::string_view text) {
    VoiceResponse* response = findMutable(id);
    if (response == nullptr) {
        return false;
    }
    if (generation == kInvalidGenerationId || generation != response->m_expectedGeneration) {
        pushEvent(VoiceEventKind::StaleOutputRejected, id, std::string(text));
        return false;
    }
    response->m_generatedText.append(text);
    return true;
}

bool VoiceSession::finishGeneration(ResponseId id, GenerationId generation) {
    VoiceResponse* response = findMutable(id);
    if (response == nullptr) {
        return false;
    }
    if (generation == kInvalidGenerationId || generation != response->m_expectedGeneration) {
        pushEvent(VoiceEventKind::StaleOutputRejected, id, "finish");
        return false;
    }
    response->m_generationComplete = true;
    response->m_expectedGeneration = kInvalidGenerationId;
    pushEvent(VoiceEventKind::GenerationFinished, id);
    if (response->m_state == ResponseState::Speaking
        && response->m_spokenOffset == response->m_generatedText.size()) {
        response->m_state = ResponseState::Completed;
        pushEvent(VoiceEventKind::ResponseCompleted, id);
    }
    return true;
}

bool VoiceSession::advancePlayback(ResponseId id, std::size_t spokenOffset) {
    VoiceResponse* response = findMutable(id);
    if (response == nullptr || !isActiveState(response->m_state)) {
        return false;
    }
    if (spokenOffset < response->m_spokenOffset || spokenOffset > response->m_generatedText.size()) {
        return false;
    }
    if (response->m_state == ResponseState::Generating) {
        response->m_state = ResponseState::Speaking;
        pushEvent(VoiceEventKind::SpeakingStarted, id);
    }
    response->m_spokenOffset = spokenOffset;
    if (response->m_generationComplete
        && response->m_spokenOffset == response->m_generatedText.size()) {
        response->m_state = ResponseState::Completed;
        pushEvent(VoiceEventKind::ResponseCompleted, id);
    }
    return true;
}

bool VoiceSession::pause(ResponseId id, std::string reason) {
    VoiceResponse* response = findMutable(id);
    if (response == nullptr || !isActiveState(response->m_state)) {
        return false;
    }
    response->m_state = ResponseState::Paused;
    response->m_expectedGeneration = kInvalidGenerationId;
    pushEvent(VoiceEventKind::PlaybackPaused, id, std::move(reason));
    return true;
}

std::optional<GenerationId> VoiceSession::resume(ResponseId id) {
    VoiceResponse* response = findMutable(id);
    if (response == nullptr || response->m_state != ResponseState::Paused || otherResponseActive(id)) {
        return std::nullopt;
    }
    GenerationId generation = kInvalidGenerationId;
    if (response->m_generationComplete) {
        response->m_state = ResponseState::Speaking;
    } else {
        generation = nextGeneration();
        response->m_expectedGeneration = generation;
        response->m_state = response->m_spokenOffset > 0 ? ResponseState::Speaking
                                                         : ResponseState::Generating;
    }
    pushEvent(VoiceEventKind::PlaybackResumed, id);
    return generation;
}

bool VoiceSession::interrupt(ResponseId id, std::string capturedUserText) {
    VoiceResponse* response = findMutable(id);
    if (response == nullptr || !isActiveState(response->m_state)) {
        return false;
    }
    InterruptionRecord record;
    record.spokenOffset = response->m_spokenOffset;
    record.capturedUserText = std::move(capturedUserText);
    response->m_interruptions.push_back(std::move(record));
    response->m_expectedGeneration = kInvalidGenerationId;
    response->m_state = ResponseState::Interrupted;
    pushEvent(VoiceEventKind::Interrupted, id, response->m_interruptions.back().capturedUserText);
    return true;
}

std::optional<GenerationId> VoiceSession::resolveInterruption(ResponseId id,
                                                              InterruptionIntent intent,
                                                              std::string_view revisedRemainder) {
    VoiceResponse* response = findMutable(id);
    if (response == nullptr || response->m_state != ResponseState::Interrupted
        || response->m_interruptions.empty()) {
        return std::nullopt;
    }
    InterruptionRecord& record = response->m_interruptions.back();
    record.resolved = true;
    record.intent = intent;
    pushEvent(VoiceEventKind::InterruptionResolved, id, toString(intent));

    switch (intent) {
    case InterruptionIntent::Pause:
    case InterruptionIntent::Question:
        response->m_state = ResponseState::Paused;
        return kInvalidGenerationId;
    case InterruptionIntent::Replacement:
        response->m_state = ResponseState::Cancelled;
        pushEvent(VoiceEventKind::ResponseCancelled, id);
        return kInvalidGenerationId;
    case InterruptionIntent::Correction:
        response->m_generatedText.resize(record.spokenOffset);
        if (revisedRemainder.empty()) {
            response->m_generationComplete = false;
        } else {
            response->m_generatedText.append(revisedRemainder);
            response->m_generationComplete = true;
        }
        response->m_state = ResponseState::Paused;
        return kInvalidGenerationId;
    case InterruptionIntent::Resume:
        response->m_state = ResponseState::Paused;
        return resume(id);
    }
    return std::nullopt;
}

bool VoiceSession::complete(ResponseId id) {
    VoiceResponse* response = findMutable(id);
    if (response == nullptr || !response->m_generationComplete) {
        return false;
    }
    if (!isActiveState(response->m_state) && response->m_state != ResponseState::Paused) {
        return false;
    }
    response->m_state = ResponseState::Completed;
    response->m_expectedGeneration = kInvalidGenerationId;
    pushEvent(VoiceEventKind::ResponseCompleted, id);
    return true;
}

bool VoiceSession::cancel(ResponseId id) {
    VoiceResponse* response = findMutable(id);
    if (response == nullptr || response->isTerminal()) {
        return false;
    }
    response->m_state = ResponseState::Cancelled;
    response->m_expectedGeneration = kInvalidGenerationId;
    pushEvent(VoiceEventKind::ResponseCancelled, id);
    return true;
}

bool VoiceSession::fail(ResponseId id, std::string error) {
    VoiceResponse* response = findMutable(id);
    if (response == nullptr || response->isTerminal()) {
        return false;
    }
    response->m_state = ResponseState::Failed;
    response->m_expectedGeneration = kInvalidGenerationId;
    response->m_error = std::move(error);
    pushEvent(VoiceEventKind::ResponseFailed, id, response->m_error);
    return true;
}

const VoiceResponse* VoiceSession::find(ResponseId id) const noexcept {
    for (const VoiceResponse& response : m_responses) {
        if (response.m_id == id) {
            return &response;
        }
    }
    return nullptr;
}

ResponseId VoiceSession::activeResponseId() const noexcept {
    for (const VoiceResponse& response : m_responses) {
        if (isActiveState(response.m_state)) {
            return response.m_id;
        }
    }
    return kInvalidResponseId;
}

VoiceResponse* VoiceSession::findMutable(ResponseId id) noexcept {
    for (VoiceResponse& response : m_responses) {
        if (response.m_id == id) {
            return &response;
        }
    }
    return nullptr;
}

bool VoiceSession::otherResponseActive(ResponseId id) const noexcept {
    for (const VoiceResponse& response : m_responses) {
        if (response.m_id != id && isActiveState(response.m_state)) {
            return true;
        }
    }
    return false;
}

void VoiceSession::pushEvent(VoiceEventKind kind, ResponseId id, std::string detail) {
    m_events.push_back({kind, id, std::move(detail)});
}

GenerationId VoiceSession::nextGeneration() noexcept {
    return m_nextGenerationId++;
}

} // namespace kestrel::core
