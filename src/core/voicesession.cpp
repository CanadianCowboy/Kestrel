#include "core/voicesession.h"

#include <algorithm>
#include <cctype>
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
            // A decimal point is not a sentence boundary.
            if (c == '.' && i > start && i + 1 < text.size()
                && std::isdigit(static_cast<unsigned char>(text[i - 1]))
                && std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
                continue;
            }
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

// Rebuild a small projection of the generated text, keeping each spoken byte's
// source end offset. The raw transcript is never changed, and a split Markdown
// construct is held until it closes instead of leaking code/URLs into speech.
struct ReadableText {
    std::string text;
    std::vector<std::size_t> sourceEnds;
    std::size_t consumed = 0;
};

ReadableText readableText(std::string_view source, bool complete) {
    ReadableText out;
    const auto emit = [&out](char c, std::size_t end) {
        out.text += c;
        out.sourceEnds.push_back(end);
    };
    bool fenced = false;
    char fenceChar = 0;
    std::size_t fenceLength = 0;
    for (std::size_t i = 0; i < source.size();) {
        const bool lineStart = i == 0 || source[i - 1] == '\n';
        if (lineStart) {
            std::size_t first = i;
            while (first < source.size() && (source[first] == ' ' || source[first] == '\t')) {
                ++first;
            }
            std::size_t run = first;
            while (run < source.size() && (source[run] == '`' || source[run] == '~')
                   && source[run] == source[first]) {
                ++run;
            }
            const std::size_t lineEnd = source.find('\n', i);
            const bool lineReady = lineEnd != std::string_view::npos || complete;
            const std::size_t end = lineEnd == std::string_view::npos ? source.size() : lineEnd + 1;
            if ((run - first >= 3 && !fenced)
                || (fenced && first < source.size() && source[first] == fenceChar
                    && run - first >= fenceLength)) {
                if (!lineReady) { break; }
                if (!fenced) {
                    fenceChar = source[first];
                    fenceLength = run - first;
                }
                fenced = !fenced;
                i = end;
                out.consumed = i;
                emit('\n', i);
                continue;
            }
            if (fenced || (first < source.size() && source[first] == '|')
                || (first - i >= 4 && first < source.size())) {
                if (!lineReady) { break; }
                i = end;
                out.consumed = i;
                emit('\n', i);
                continue;
            }
            if (first < source.size() && source[first] == '#') {
                std::size_t marker = first;
                while (marker < source.size() && source[marker] == '#') { ++marker; }
                if (marker == source.size() && !complete) { break; }
                if (marker < source.size() && isWhitespace(source[marker])) {
                    i = marker + 1;
                    out.consumed = i;
                    continue;
                }
            }
            if (first < source.size() && (source[first] == '-' || source[first] == '*'
                                         || source[first] == '+' || source[first] == '>')) {
                if (first + 1 == source.size() && !complete) { break; }
                if (first + 1 < source.size() && isWhitespace(source[first + 1])) {
                    i = first + 2;
                    out.consumed = i;
                    continue;
                }
            }
            std::size_t marker = first;
            while (marker < source.size() && std::isdigit(static_cast<unsigned char>(source[marker]))) {
                ++marker;
            }
            if (marker > first && marker < source.size()
                && (source[marker] == '.' || source[marker] == ')')) {
                if (marker + 1 == source.size() && !complete) { break; }
                if (marker + 1 < source.size() && isWhitespace(source[marker + 1])) {
                    i = marker + 2;
                    out.consumed = i;
                    continue;
                }
            }
        }
        if (source[i] == '`') {
            std::size_t run = i;
            while (run < source.size() && source[run] == '`') { ++run; }
            const auto close = source.find(source.substr(i, run - i), run);
            if (close == std::string_view::npos && !complete) { break; }
            i = close == std::string_view::npos ? source.size() : close + run - i;
            out.consumed = i;
            continue;
        }
        if (source[i] == '[' || (source[i] == '!' && i + 1 < source.size() && source[i + 1] == '[')) {
            const bool image = source[i] == '!';
            const std::size_t label = i + (image ? 2 : 1);
            const auto close = source.find(']', label);
            if (close == std::string_view::npos && !complete) { break; }
            if (close != std::string_view::npos) {
                if (close + 1 == source.size() && !complete) { break; }
                if (close + 1 < source.size() && source[close + 1] == '(') {
                    const auto targetEnd = source.find(')', close + 2);
                    if (targetEnd == std::string_view::npos && !complete) { break; }
                    if (!image) {
                        for (std::size_t j = label; j < close; ++j) { emit(source[j], j + 1); }
                    }
                    i = targetEnd == std::string_view::npos ? source.size() : targetEnd + 1;
                    out.consumed = i;
                    continue;
                }
                const auto citation = source.substr(label, close - label);
                if (citation.starts_with('^') || (!citation.empty()
                    && citation.find_first_not_of("0123456789, -") == std::string_view::npos)) {
                    i = close + 1;
                    out.consumed = i;
                    continue;
                }
            }
        }
        // Hold complete words so a URL, filename or path split across tokens
        // cannot have its first half spoken before the rest identifies it.
        if (!isWhitespace(source[i]) && (i == 0 || isWhitespace(source[i - 1]))) {
            std::size_t end = i;
            while (end < source.size() && !isWhitespace(source[end])) { ++end; }
            if (end == source.size() && !complete) { break; }
            auto word = source.substr(i, end - i);
            while (!word.empty() && (word.back() == '.' || word.back() == ','
                                     || word.back() == ')' || word.back() == '>')) {
                word.remove_suffix(1);
            }
            if (word.starts_with("http://") || word.starts_with("https://") || word.starts_with("www.")
                || word.find('/') != std::string_view::npos || word.find('\\') != std::string_view::npos
                || word.ends_with(".yaml") || word.ends_with(".yml") || word.ends_with(".json")
                || word.ends_with(".py") || word.ends_with(".txt") || word.ends_with(".h")) {
                i = end;
                out.consumed = i;
                continue;
            }
        }
        if (source[i] == '*' || source[i] == '~'
            || (source[i] == '_' && (i == 0 || !std::isalnum(static_cast<unsigned char>(source[i - 1]))))) {
            ++i;
            out.consumed = i;
            continue;
        }
        emit(source[i], i + 1);
        out.consumed = ++i;
    }
    return out;
}

std::optional<SpeechSegment> readableSegment(const VoiceResponse& response, std::size_t base,
                                             const VoicePersona& persona, int openingPause) {
    const ReadableText readable = readableText(response.generatedText(), response.generationComplete());
    std::size_t first = 0;
    while (first < readable.text.size()
           && (readable.sourceEnds[first] <= base || isWhitespace(readable.text[first]))) { ++first; }
    const auto remainder = std::string_view(readable.text).substr(first);
    if (remainder.empty()) {
        if (readable.consumed > base) {
            return SpeechSegment{{}, base, readable.consumed, 0, base == 0};
        }
        return std::nullopt;
    }
    const auto end = clauseEndFrom(remainder, 0);
    const char last = remainder[end - 1];
    if (!response.generationComplete() && end == remainder.size()
        && !isSentenceTerminator(last) && !isClauseSoftBreak(last)
        && !isTrailingPunctuation(last)) {
        return std::nullopt;
    }
    const int pause = openingPause != 0 ? openingPause
        : base == 0 ? persona.leadInMs
                    : pauseAfterClause(response.generatedText(), base, persona);
    return SpeechSegment{std::string(remainder.substr(0, end)),
                         first == 0 ? 0 : readable.sourceEnds[first - 1],
                         readable.sourceEnds[first + end - 1], pause, base == 0};
}

} // namespace

SpokenIntent classifySpokenIntent(std::string_view text) {
    std::string normalized;
    for (const unsigned char c : text) {
        // The rule vocabulary is explicitly English/ASCII. Never discard an
        // unrecognized Unicode word and mistake the remaining "okay" for intent.
        if (c >= 128) { return SpokenIntent::Ambiguous; }
        if (std::isalnum(c)) {
            normalized += static_cast<char>(std::tolower(c));
        } else if ((isWhitespace(static_cast<char>(c)) || c == '-') && !normalized.empty() && normalized.back() != ' ') {
            normalized += ' ';
        }
    }
    if (!normalized.empty() && normalized.back() == ' ') { normalized.pop_back(); }
    if (normalized.starts_with("kestrel ")) { normalized.erase(0, 8); }
    if (normalized.starts_with("please ")) { normalized.erase(0, 7); }
    if (normalized.ends_with(" please")) { normalized.resize(normalized.size() - 7); }
    constexpr std::string_view acknowledgements[] = {
        "ok", "okay", "yeah", "yes", "yep", "mm hm", "mhm", "mmhmm", "uh huh", "got it", "right", "i see",
        "okay thanks", "ok thanks", "that makes sense", "makes sense", "im listening", "i am listening",
        "keep going", "go on", "understood", "sure", "alright", "all right", "thank you", "thanks"
    };
    if (std::find(std::begin(acknowledgements), std::end(acknowledgements), normalized)
        != std::end(acknowledgements)) { return SpokenIntent::Acknowledgement; }
    if (normalized == "pause" || normalized == "pause speech" || normalized == "pause audio"
        || normalized == "hold on" || normalized == "wait a moment" || normalized == "wait") {
        return SpokenIntent::Pause;
    }
    if (normalized == "resume" || normalized == "resume speech" || normalized == "resume audio"
        || normalized == "continue speaking") { return SpokenIntent::Resume; }
    if (normalized == "stop" || normalized == "stop speaking" || normalized == "stop audio"
        || normalized == "be quiet" || normalized == "quiet") { return SpokenIntent::StopAudio; }
    if (normalized == "cancel" || normalized == "cancel answer" || normalized == "cancel response"
        || normalized == "stop generating" || normalized == "cancel generation") { return SpokenIntent::Cancel; }
    return SpokenIntent::Ambiguous;
}

bool isListeningAcknowledgement(std::string_view text) {
    return classifySpokenIntent(text) == SpokenIntent::Acknowledgement;
}

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

float paceFor(const VoicePersona& persona) noexcept {
    // The warmth the engine is left alone at, which is the voice persona's own
    // default and the resting point of PersonaState::warmth, and how far the pace
    // moves across the rest of the range. About a tenth of the pace in each
    // direction: enough to hear, not enough to sound like a different person. An
    // assistant that changed register noticeably every time it changed its mind
    // would be tiring rather than alive.
    constexpr float kNeutralWarmth = 0.6F;
    constexpr float kWarmthRange = 0.30F;
    // rate is already the engine's speed multiplier (1.0 is its natural pace).
    // The former 0.5 offset pushed the default to 1.46, which is why playback
    // sounded hurried.
    return persona.rate - kWarmthRange * (persona.warmth - kNeutralWarmth);
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
    return readableSegment(*response, response->spokenOffset(), m_voicePersona, openingPauseMs);
}

std::optional<SpeechSegment> VoiceSession::peekSpeechSegment(ResponseId id) const {
    const VoiceResponse* response = find(id);
    if (response == nullptr) {
        return std::nullopt;
    }
    const auto first = nextSpeechSegment(id);
    if (!first.has_value()) {
        return std::nullopt;
    }
    return readableSegment(*response, first->endOffset, m_voicePersona, 0);
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
