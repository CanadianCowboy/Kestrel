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

bool isClauseSoftBreak(char c) noexcept {
    return c == ',' || c == ';' || c == ':' || c == '\n';
}

// A clause shorter than this is not worth a pause of its own: "Well, no" split
// in two sounds like a stutter rather than a breath.
constexpr std::size_t kMinClauseChars = 16;

// Closing characters that trail a terminator without ending the clause, e.g.
// the quote in: He said "go." Then he left.
constexpr std::string_view kClosingQuote = "\xe2\x80\x9d";

bool isTrailingPunctuation(char c) noexcept {
    return c == '"' || c == '\'' || c == ')' || c == ']' || c == '}';
}

std::size_t skipSpace(std::string_view text, std::size_t i) noexcept {
    while (i < text.size() && isWhitespace(text[i])) {
        ++i;
    }
    return i;
}

// End of the clause beginning at `start`, or text.size().
std::size_t clauseEndFrom(std::string_view text, std::size_t start) noexcept {
    for (std::size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (isSentenceTerminator(c)) {
            std::size_t end = i + 1;
            while (end < text.size()) {
                if (isTrailingPunctuation(text[end])) {
                    ++end;
                } else if (text.substr(end).starts_with(kClosingQuote)) {
                    end += kClosingQuote.size();
                } else {
                    break;
                }
            }
            return end;
        }
        if (isClauseSoftBreak(c) && i - start >= kMinClauseChars) {
            return i + 1;
        }
    }
    return text.size();
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

const VoicePersona& defaultVoicePersona() noexcept {
    // A function-local static rather than an inline variable, so the default is
    // constructed once and every translation unit agrees on it.
    static const VoicePersona kDefault{};
    return kDefault;
}

std::size_t clauseStartBefore(std::string_view text, std::size_t offset) noexcept {
    const std::size_t limit = std::min(offset, text.size());
    std::size_t start = 0;
    for (std::size_t i = 0; i < limit; ++i) {
        if (isSentenceTerminator(text[i]) || isClauseSoftBreak(text[i])) {
            const std::size_t next = skipSpace(text, i + 1);
            // A soft break only starts a clause if what precedes it was long
            // enough to be a clause, matching clauseEndFrom.
            if (isSentenceTerminator(text[i]) || i - start >= kMinClauseChars) {
                start = next;
            }
        }
    }
    return start;
}

int pauseAfterClause(std::string_view text, std::size_t clauseEnd,
                     const VoicePersona& persona) noexcept {
    if (clauseEnd == 0 || clauseEnd > text.size()) {
        return persona.clausePauseMs;
    }
    // Look at the last meaningful character of the clause: a full stop earns a
    // longer breath than a comma.
    std::size_t last = clauseEnd;
    while (last > 0 && isWhitespace(text[last - 1])) {
        --last;
    }
    if (last == 0) {
        return persona.clausePauseMs;
    }
    const char c = text[last - 1];
    if (isSentenceTerminator(c) || isTrailingPunctuation(c)
        || text.substr(0, last).ends_with(kClosingQuote)) {
        return persona.sentencePauseMs;
    }
    return persona.clausePauseMs;
}

std::vector<SpeechSegment> planSpeech(std::string_view text, const VoicePersona& persona,
                                      int openingPauseMs) {
    std::vector<SpeechSegment> segments;
    std::size_t i = skipSpace(text, 0);
    int previousPause = openingPauseMs;
    while (i < text.size()) {
        const std::size_t end = clauseEndFrom(text, i);
        SpeechSegment segment;
        segment.text = std::string(text.substr(i, end - i));
        segment.startOffset = i;
        segment.endOffset = end;
        // The first segment carries the caller's opening pause; every later one
        // carries the pause implied by the clause before it.
        segment.leadingPauseMs = previousPause;
        segment.isFirst = segments.empty();
        // The next segment waits for whatever the clause just ended with. The
        // final clause sets a pause nobody will use, which is harmless and
        // keeps the loop free of a special case.
        previousPause = pauseAfterClause(text, end, persona);
        segments.push_back(std::move(segment));
        i = skipSpace(text, end);
    }
    return segments;
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

void VoiceSession::setVoicePersona(VoicePersona persona) {
    m_voicePersona = std::move(persona);
}

const VoicePersona& VoiceSession::voicePersona() const noexcept {
    return m_voicePersona;
}

std::optional<SpeechSegment> VoiceSession::nextSpeechSegment(ResponseId id,
                                                             int openingPauseMs) const {
    const VoiceResponse* response = find(id);
    if (response == nullptr) {
        return std::nullopt;
    }
    const std::string_view remainder = response->unspokenText();
    if (remainder.empty()) {
        return std::nullopt;
    }

    const std::size_t base = response->spokenOffset();
    // A zero opening pause means "continue what was already being said", not
    // "start abruptly". Each call replans from the remainder, so without this
    // every clause after the first would be spoken with no gap at all.
    int leadIn = openingPauseMs;
    if (leadIn == 0) {
        leadIn = base == 0
                     ? m_voicePersona.leadInMs
                     : pauseAfterClause(response->generatedText(), base, m_voicePersona);
    }

    // Plan against the remainder so offsets are relative to it, then shift them
    // back onto the response timeline: the caller advances the spoken cursor
    // with endOffset and the accounting stays exact.
    const std::vector<SpeechSegment> planned = planSpeech(remainder, m_voicePersona, leadIn);
    if (planned.empty()) {
        return std::nullopt;
    }
    SpeechSegment segment = planned.front();
    segment.isFirst = base == 0;
    segment.startOffset += base;
    segment.endOffset += base;
    return segment;
}

std::optional<SpeechSegment> VoiceSession::peekSpeechSegment(ResponseId id) const {
    const VoiceResponse* response = find(id);
    if (response == nullptr) {
        return std::nullopt;
    }
    // The next segment is planned from everything still unspoken. Once that
    // segment has been taken, the remainder is what follows it -- so the peek
    // plans against the remainder minus the first planned segment, which is
    // exactly the text the caller is about to be handed.
    const std::string_view remainder = response->unspokenText();
    if (remainder.empty()) {
        return std::nullopt;
    }
    const std::size_t base = response->spokenOffset();
    const std::vector<SpeechSegment> planned = planSpeech(
        remainder, m_voicePersona, pauseAfterClause(
                                 response->generatedText(), base, m_voicePersona));
    if (planned.size() < 2) {
        return std::nullopt;
    }

    SpeechSegment ahead = planned[1];
    // Every planned offset is relative to the remainder, so the shift onto the
    // response timeline is the same one applied to the segment that does get
    // spoken. Adding the first segment's length as well would double-count it:
    // planned[1] already starts after it.
    ahead.startOffset += base;
    ahead.endOffset += base;
    ahead.isFirst = false;
    return ahead;
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
