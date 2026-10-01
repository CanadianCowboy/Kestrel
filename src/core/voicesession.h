#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::core {

using ResponseId = std::uint64_t;
using GenerationId = std::uint64_t;

inline constexpr ResponseId kInvalidResponseId = 0;
inline constexpr GenerationId kInvalidGenerationId = 0;

enum class ResponseState {
    Queued,
    Generating,
    Speaking,
    Paused,
    Interrupted,
    Completed,
    Cancelled,
    Failed,
};

enum class InterruptionIntent {
    Pause,
    Correction,
    Question,
    Replacement,
    Resume,
};

enum class VoiceEventKind {
    ResponseQueued,
    GenerationStarted,
    GenerationFinished,
    SpeakingStarted,
    PlaybackPaused,
    PlaybackResumed,
    Interrupted,
    InterruptionResolved,
    StaleOutputRejected,
    ResponseCompleted,
    ResponseCancelled,
    ResponseFailed,
};

struct VoiceEvent {
    VoiceEventKind kind;
    ResponseId responseId;
    std::string detail;
};

struct InterruptionRecord {
    std::size_t spokenOffset = 0;
    std::string capturedUserText;
    bool resolved = false;
    InterruptionIntent intent = InterruptionIntent::Pause;
};

[[nodiscard]] const char* toString(ResponseState state) noexcept;
[[nodiscard]] const char* toString(InterruptionIntent intent) noexcept;

// How Kestrel sounds, and how it paces itself.
//
// TTS is not implemented yet, but these decisions belong here rather than in the
// UI: a voice that clips its clauses together or hurries the first syllable is
// a property of the response timeline, and the same decisions apply whether the
// audio is synthesized locally or handed to a system voice later.
struct VoicePersona {
    // Identifies the intended voice to a synthesizer. Descriptive rather than a
    // vendor voice name, so nothing here breaks when a platform is missing.
    std::string voiceId = "kestrel-warm-neutral";
    // 1.0 is the voice's natural pace; below is deliberate, unhurried.
    float rate = 0.96F;
    // Semitones. Neutral, because an assistant that sounds emphatic all the time
    // sounds like it is only sometimes paying attention.
    float pitch = 0.0F;
    // 0 flat .. 1 warm. The one pacing value a user hears rather than reads, and
    // the only one that moves: the app fills it from the persona's warmth dial
    // at the start of each response, so a mood that has shifted is audible in
    // the next reply. Read it as pace rather than timbre, because pace is the
    // thing every synthesizer here can actually change -- see
    // core::PersonaState::warmth.
    float warmth = 0.6F;
    // The gap before the very first clause.
    int leadInMs = 90;
    // Between clauses: after a comma, a list item, a "which" clause.
    int clausePauseMs = 130;
    // After a full stop. The difference between a clause and a sentence is most
    // of what makes speech sound like a person rather than a document.
    int sentencePauseMs = 300;
};

[[nodiscard]] const VoicePersona& defaultVoicePersona() noexcept;

// The pace a synthesizer should be asked for. The persona's rate, shifted by
// however far its warmth has moved from neutral: warmer is slower, cooler is
// brisker.
//
// A free function in core rather than an expression inside a backend, because it
// is a statement about the voice and not about one engine. Kokoro and Piper both
// take a speed, and a warmth that moved them by different amounts would be a
// warmth the user could hear changing between machines.
//
// Written as a deviation rather than an absolute on purpose: at neutral warmth
// it returns the 0.5 + rate the engine has always been given, so a persona that
// has not drifted sounds exactly as it did before warmth was wired up. The first
// thing anyone would notice otherwise is the voice changing for no reason.
[[nodiscard]] float paceFor(const VoicePersona& persona) noexcept;

// One clause, ready to hand to a synthesizer.
struct SpeechSegment {
    std::string text;
    // Offsets into the text this was planned from. Absolute in the response
    // timeline when it came from VoiceSession::nextSpeechSegment, so playback
    // can advance the spoken cursor by exactly what was spoken.
    std::size_t startOffset = 0;
    std::size_t endOffset = 0;
    // The micro-pause that precedes this segment. Zero for the first one: there
    // is nothing before it to leave a gap after.
    int leadingPauseMs = 0;
    bool isFirst = false;
};

// Returns the start offset of the clause containing `offset`, so playback cuts
// on a clause boundary instead of mid-thought. Mirrors sentenceStartBefore at a
// finer grain, and is what makes an interrupted reply resumable mid-paragraph.
[[nodiscard]] std::size_t clauseStartBefore(std::string_view text, std::size_t offset) noexcept;

// The micro-pause that belongs after the clause ending at `clauseEnd`. Derived
// from the punctuation there, so a comma and a full stop get different gaps.
[[nodiscard]] int pauseAfterClause(std::string_view text, std::size_t clauseEnd,
                                   const VoicePersona& persona) noexcept;

// Splits text into clause-sized segments with the micro-pauses that belong
// between them. `openingPauseMs` is applied to the first segment, which is how
// an acknowledgement cue leaves a deliberate gap before the answer starts.
[[nodiscard]] std::vector<SpeechSegment> planSpeech(
    std::string_view text,
    const VoicePersona& persona = defaultVoicePersona(),
    int openingPauseMs = 0);

// Returns the start offset of the sentence containing `offset`, so playback can
// resume from a clean semantic boundary instead of mid-sentence.
[[nodiscard]] std::size_t sentenceStartBefore(std::string_view text, std::size_t offset) noexcept;

// One assistant response with overlapping generation and playback tracking.
// Instances are owned and mutated exclusively by VoiceSession.
class VoiceResponse {
public:
    [[nodiscard]] ResponseId id() const noexcept { return m_id; }
    [[nodiscard]] ResponseState state() const noexcept { return m_state; }
    [[nodiscard]] const std::string& generatedText() const noexcept { return m_generatedText; }
    [[nodiscard]] std::size_t spokenOffset() const noexcept { return m_spokenOffset; }
    [[nodiscard]] std::string_view spokenText() const noexcept;
    [[nodiscard]] std::string_view unspokenText() const noexcept;
    [[nodiscard]] bool generationComplete() const noexcept { return m_generationComplete; }
    [[nodiscard]] const std::vector<InterruptionRecord>& interruptions() const noexcept { return m_interruptions; }
    [[nodiscard]] const std::string& error() const noexcept { return m_error; }
    [[nodiscard]] const std::string& runtimeContext() const noexcept { return m_runtimeContext; }
    [[nodiscard]] bool isTerminal() const noexcept;

private:
    friend class VoiceSession;

    ResponseId m_id = kInvalidResponseId;
    ResponseState m_state = ResponseState::Queued;
    std::string m_generatedText;
    std::size_t m_spokenOffset = 0;
    bool m_generationComplete = false;
    GenerationId m_expectedGeneration = kInvalidGenerationId;
    std::vector<InterruptionRecord> m_interruptions;
    std::string m_error;
    std::string m_runtimeContext;
};

// Deterministic state machine for the voice conversation loop.
//
// It coordinates one response timeline at a time (at most one response is
// Generating or Speaking), preserves interrupted responses for later
// continuation, and rejects stale generation output after an interruption,
// pause, cancellation, or revision. All mutations append structured
// VoiceEvents so behavior is inspectable without parsing UI text.
//
// Pointers returned by find() are invalidated by queueResponse().
class VoiceSession {
public:
    // Creates a preserved response record. `runtimeContext` captures the
    // conversation context and runtime settings used for the response.
    ResponseId queueResponse(std::string runtimeContext = {});

    // Queued -> Generating. Returns the generation token append/finish calls
    // must present, or kInvalidGenerationId if the response cannot start
    // (unknown id, wrong state, or another response is active).
    [[nodiscard]] GenerationId beginGeneration(ResponseId id);

    // Appends streamed text. Returns false and records StaleOutputRejected
    // when `generation` is no longer the live generation for this response.
    bool appendText(ResponseId id, GenerationId generation, std::string_view text);

    // Marks generation finished. Completes the response if playback has
    // already consumed all generated text.
    bool finishGeneration(ResponseId id, GenerationId generation);

    // Advances the spoken cursor (monotonic, clamped to generated length).
    // The first advance moves Generating -> Speaking. Completes the response
    // when generation is finished and all text has been spoken.
    bool advancePlayback(ResponseId id, std::size_t spokenOffset);

    // Generating/Speaking -> Paused without an interruption record, e.g. a UI
    // pause button or an audio-device failure the app wants to recover from.
    bool pause(ResponseId id, std::string reason = {});

    // Paused -> Speaking (or Generating when nothing was spoken yet and
    // generation is incomplete). Refused (nullopt) if another response is
    // active. Returns a fresh generation token when generation must continue,
    // kInvalidGenerationId when the remaining text is already generated.
    [[nodiscard]] std::optional<GenerationId> resume(ResponseId id);

    // Barge-in: Generating/Speaking -> Interrupted. Preserves the spoken
    // cutoff and the user's captured words, and invalidates the live
    // generation so late tokens are rejected as stale.
    bool interrupt(ResponseId id, std::string capturedUserText);

    // Decides what an interruption meant. Pause/Question preserve the
    // response (-> Paused). Replacement discards it (-> Cancelled).
    // Correction truncates at the spoken cutoff and either installs
    // `revisedRemainder` as the new remainder or reopens generation for it.
    // Resume immediately continues playback (see resume()).
    [[nodiscard]] std::optional<GenerationId> resolveInterruption(ResponseId id,
                                                                  InterruptionIntent intent,
                                                                  std::string_view revisedRemainder = {});

    // Marks a fully generated response as delivered, e.g. text-only fallback
    // when audio is unavailable.
    bool complete(ResponseId id);
    bool cancel(ResponseId id);
    bool fail(ResponseId id, std::string error);

    // Pacing for this session. Independent of the responses themselves, so the
    // voice can be adjusted without disturbing the timeline.
    void setVoicePersona(VoicePersona persona);
    [[nodiscard]] const VoicePersona& voicePersona() const noexcept;

    // The next unspoken clause of `id`, with the micro-pause that precedes it
    // and absolute offsets into the response, so the caller can speak it and
    // then advance the spoken cursor to exactly segment.endOffset.
    //
    // This is the whole reason speech can start before generation finishes: the
    // first clause is playable the moment those tokens exist, so the reply is
    // already audible while the rest is still being decoded.
    //
    // `openingPauseMs` overrides the gap before the clause. Leave it at zero for
    // a continuation: the pause implied by whatever was last spoken is used
    // instead, so a clause-by-clause caller does not have to track it. Pass a
    // real value when something was said between the clauses -- an
    // acknowledgement cue, most often.
    //
    // Returns nullopt when the response is fully spoken or does not exist.
    [[nodiscard]] std::optional<SpeechSegment> nextSpeechSegment(ResponseId id,
                                                                  int openingPauseMs = 0) const;
    // The segment that would come after `nextSpeechSegment` returns right now,
    // without moving the spoken cursor. Lets a caller warn a speech engine about
    // the sentence it is about to be given, so the engine can start work on it
    // while the previous one is still playing. Returns nullopt at the end of the
    // response, or before anything has been handed out yet.
    [[nodiscard]] std::optional<SpeechSegment> peekSpeechSegment(ResponseId id) const;

    [[nodiscard]] const VoiceResponse* find(ResponseId id) const noexcept;
    [[nodiscard]] ResponseId activeResponseId() const noexcept;
    [[nodiscard]] const std::vector<VoiceResponse>& responses() const noexcept { return m_responses; }
    [[nodiscard]] const std::vector<VoiceEvent>& events() const noexcept { return m_events; }

private:
    [[nodiscard]] VoiceResponse* findMutable(ResponseId id) noexcept;
    [[nodiscard]] bool otherResponseActive(ResponseId id) const noexcept;
    void pushEvent(VoiceEventKind kind, ResponseId id, std::string detail = {});
    [[nodiscard]] GenerationId nextGeneration() noexcept;

    std::vector<VoiceResponse> m_responses;
    std::vector<VoiceEvent> m_events;
    VoicePersona m_voicePersona = defaultVoicePersona();
    ResponseId m_nextResponseId = 1;
    GenerationId m_nextGenerationId = 1;
};

} // namespace kestrel::core
