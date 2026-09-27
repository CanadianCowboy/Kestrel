#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "core/persona.h"
#include "core/voicesession.h"

namespace kestrel::core {

// What the user did last, recorded as a meaning rather than as a widget event.
// The UI animates a state; it does not need to know which control caused it.
enum class UserAction {
    Silent,
    Typed,
    Spoke,
    Interrupted,
    Paused,
    Resumed,
    Corrected,
    Returned,
};

// What Kestrel did last.
enum class AssistantAction {
    Idle,
    Acknowledging,
    Thinking,
    Speaking,
    Reflecting,
    Preparing,
    Waiting,
};

[[nodiscard]] const char* toString(UserAction action) noexcept;
[[nodiscard]] const char* toString(AssistantAction action) noexcept;

// The assistant mood, as plain booleans.
//
// This is a struct of flags on purpose. A single "mood score" would push the
// threshold decision into the UI, and every surface would then invent its own
// idea of what counts as calm. Flags can be bound and compared directly.
struct MoodFlags {
    bool calm = true;
    bool attentive = true;
    bool curious = false;
    bool proactive = false;
    bool busy = false;
    bool present = true;
};

// One frame of "what is going on", for the UI to animate against.
struct PresenceSnapshot {
    UserAction lastUserAction = UserAction::Silent;
    AssistantAction lastAssistantAction = AssistantAction::Idle;
    ResponseState voiceState = ResponseState::Queued;
    bool generating = false;
    PersonaMood mood = PersonaMood::Calm;
    MoodFlags flags;
    std::uint64_t lastChangeMs = 0;
};

// The presence engine.
//
// A small subsystem that tracks what the user did, what the assistant did, the
// voice and generation states, and the mood, so the interface can animate
// meaning instead of raw events. It is deliberately a plain struct plus a few
// setters: there is no timer, no thread, and no Qt here, which keeps the whole
// idea testable without an event loop.
class Presence {
public:
    // Time is injected rather than read. The owner passes a monotonic
    // millisecond reading, so intensity easing is reproducible in tests and
    // this layer never reaches for a clock of its own.
    void setNow(std::uint64_t nowMs) noexcept;
    [[nodiscard]] std::uint64_t now() const noexcept;

    void noteUserAction(UserAction action) noexcept;
    void noteAssistantAction(AssistantAction action) noexcept;
    void setVoiceState(ResponseState state) noexcept;
    void setGenerating(bool generating) noexcept;

    /// Re-derives the mood flags from the drifting dials. The dials move over
    /// time; the flags are what the UI reads, so they are recomputed from the
    /// same source of truth rather than set independently and drifting apart.
    void applyPersona(const PersonaState& state) noexcept;

    // Eases the animated intensity toward the target for the current state and
    // returns the new value. Called from the owner's tick; the ease lives here
    // so the rate is part of the state machine instead of a QML animation.
    float advance();

    [[nodiscard]] const PresenceSnapshot& snapshot() const noexcept;
    /// 0..1, for glow and pulse amplitude.
    [[nodiscard]] float intensity() const noexcept;
    [[nodiscard]] bool speaking() const noexcept;
    [[nodiscard]] bool busy() const noexcept;
    /// The coarse activity, for the status line.
    [[nodiscard]] PersonaActivity activity() const noexcept;

private:
    [[nodiscard]] float targetIntensity() const noexcept;
    void touch(std::uint64_t nowMs) noexcept;

    PresenceSnapshot m_snapshot;
    float m_intensity = 0.28F;
    std::uint64_t m_lastAdvanceMs = 0;
    std::uint64_t m_nowMs = 0;
};

} // namespace kestrel::core
