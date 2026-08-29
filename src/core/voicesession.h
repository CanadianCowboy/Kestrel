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
    ResponseId m_nextResponseId = 1;
    GenerationId m_nextGenerationId = 1;
};

} // namespace kestrel::core
