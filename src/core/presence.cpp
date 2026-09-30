#include "core/presence.h"

#include <algorithm>

namespace kestrel::core {

namespace {

// Idle glow: present but unremarkable. A presence that dims to nothing reads as
// a switched-off application rather than a quiet one.
constexpr float kIdleIntensity = 0.28F;
constexpr float kPreparingIntensity = 0.66F;
constexpr float kBusyIntensity = 1.0F;

// Fraction of the remaining distance covered per second, so the ease rate is
// independent of how often the owner happens to tick.
constexpr float kEasePerSecond = 2.4F;

/// Linearly interpolates between two values using the supplied, unclamped fraction.
float lerp(float from, float to, float t) noexcept {
    return from + (to - from) * t;
}

} // namespace

/// Returns a user-action label, or unknown for an unrecognized value.
const char* toString(UserAction action) noexcept {
    switch (action) {
    case UserAction::Silent: return "silent";
    case UserAction::Typed: return "typed";
    case UserAction::Spoke: return "spoke";
    case UserAction::Interrupted: return "interrupted";
    case UserAction::Paused: return "paused";
    case UserAction::Resumed: return "resumed";
    case UserAction::Corrected: return "corrected";
    case UserAction::Returned: return "returned";
    }
    return "unknown";
}

/// Returns an assistant-action label, or unknown for an unrecognized value.
const char* toString(AssistantAction action) noexcept {
    switch (action) {
    case AssistantAction::Idle: return "idle";
    case AssistantAction::Acknowledging: return "acknowledging";
    case AssistantAction::Thinking: return "thinking";
    case AssistantAction::Speaking: return "speaking";
    case AssistantAction::Reflecting: return "reflecting";
    case AssistantAction::Preparing: return "preparing";
    case AssistantAction::Waiting: return "waiting";
    }
    return "unknown";
}

/// Sets the caller-provided monotonic millisecond time for subsequent updates.
void Presence::setNow(std::uint64_t nowMs) noexcept {
    m_nowMs = nowMs;
}

/// Returns the last time supplied by the owner in milliseconds.
std::uint64_t Presence::now() const noexcept {
    return m_nowMs;
}

/// Records a changed user action and stamps the snapshot with the supplied time.
void Presence::noteUserAction(UserAction action) noexcept {
    if (m_snapshot.lastUserAction == action) {
        return;
    }
    m_snapshot.lastUserAction = action;
    touch(m_nowMs);
}

/// Records a changed assistant action and stamps the snapshot with the supplied time.
void Presence::noteAssistantAction(AssistantAction action) noexcept {
    if (m_snapshot.lastAssistantAction == action) {
        return;
    }
    m_snapshot.lastAssistantAction = action;
    touch(m_nowMs);
}

/// Records a changed voice state and stamps the snapshot with the supplied time.
void Presence::setVoiceState(ResponseState state) noexcept {
    if (m_snapshot.voiceState == state) {
        return;
    }
    m_snapshot.voiceState = state;
    touch(m_nowMs);
}

/// Updates generation and busy flags together and timestamps changes.
void Presence::setGenerating(bool generating) noexcept {
    if (m_snapshot.generating == generating) {
        return;
    }
    m_snapshot.generating = generating;
    m_snapshot.flags.busy = generating;
    touch(m_nowMs);
}

/// Derives mood, calmness, curiosity, and initiative flags without altering generation state.
void Presence::applyPersona(const PersonaState& state) noexcept {
    m_snapshot.mood = moodFor(state);
    m_snapshot.flags.calm = m_snapshot.mood != PersonaMood::Alert;
    m_snapshot.flags.curious = m_snapshot.mood == PersonaMood::Contemplative;
    m_snapshot.flags.proactive = state.initiative >= 0.45F;
    // Busy is owned by setGenerating: it is a fact about the generation state,
    // not a feeling, and letting the dials overwrite it would make the pulse
    // stutter whenever the personality drifted mid-answer.
}

/// Selects a target glow intensity from the last assistant action.
float Presence::targetIntensity() const noexcept {
    switch (m_snapshot.lastAssistantAction) {
    case AssistantAction::Speaking:
        return kBusyIntensity;
    case AssistantAction::Thinking:
        return kBusyIntensity;
    case AssistantAction::Acknowledging:
    case AssistantAction::Preparing:
        return kPreparingIntensity;
    case AssistantAction::Reflecting:
        return lerp(kIdleIntensity, kPreparingIntensity, 0.5F);
    case AssistantAction::Idle:
    case AssistantAction::Waiting:
        break;
    }
    return kIdleIntensity;
}

/// Eases intensity using elapsed injected time; nonadvancing time leaves the value unchanged.
float Presence::advance() {
    const std::uint64_t previous = m_lastAdvanceMs;
    m_lastAdvanceMs = m_nowMs;
    // A tick before any time has passed, or a clock that jumped backwards, must
    // not divide by zero or fling the animation backwards.
    if (m_nowMs <= previous) {
        return m_intensity;
    }
    const float seconds = static_cast<float>(m_nowMs - previous) / 1000.0F;
    const float t = std::clamp(seconds * kEasePerSecond, 0.0F, 1.0F);
    m_intensity = lerp(m_intensity, targetIntensity(), t);
    return m_intensity;
}

/// Returns the current presence snapshot by reference.
const PresenceSnapshot& Presence::snapshot() const noexcept {
    return m_snapshot;
}

/// Returns the current eased glow intensity.
float Presence::intensity() const noexcept {
    return m_intensity;
}

/// Returns whether either voice state or assistant action indicates speaking.
bool Presence::speaking() const noexcept {
    return m_snapshot.voiceState == ResponseState::Speaking ||
           m_snapshot.lastAssistantAction == AssistantAction::Speaking;
}

/// Returns whether response generation is active.
bool Presence::busy() const noexcept {
    return m_snapshot.generating;
}

/// Projects the assistant action into a persona activity, defaulting to standing by.
PersonaActivity Presence::activity() const noexcept {
    switch (m_snapshot.lastAssistantAction) {
    case AssistantAction::Acknowledging:
    case AssistantAction::Preparing:
        return PersonaActivity::Preparing;
    case AssistantAction::Thinking:
        return PersonaActivity::Thinking;
    case AssistantAction::Speaking:
        return PersonaActivity::Speaking;
    case AssistantAction::Reflecting:
        return PersonaActivity::Reflecting;
    case AssistantAction::Idle:
    case AssistantAction::Waiting:
        break;
    }
    return PersonaActivity::StandingBy;
}

/// Stamps the snapshot's last-change time in milliseconds.
void Presence::touch(std::uint64_t nowMs) noexcept {
    m_snapshot.lastChangeMs = nowMs;
}

} // namespace kestrel::core
