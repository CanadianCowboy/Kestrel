#include "core/idlepersona.h"

#include <algorithm>
#include <array>

namespace kestrel::core {

namespace {

// Bound on remembered work. "Never idle" must not become a memory leak: a
// process left running for a week should hold the same handful of tasks.
constexpr std::size_t kHistoryLimit = 16;

// Dials never reach the ends of their range. A personality pinned at 1.0 has no
// room to be surprising, and one pinned at 0.0 has stopped being present.
constexpr float kDialFloor = 0.05F;

// Where the warmth dial comes to rest between nudges, and the value
// core::VoicePersona treats as neutral. Kept beside the dial floor because both
// are the loop's opinion about where a dial belongs, and because a voice that
// rests somewhere other than its own neutral would drift off it on the very
// first idle cycle.
constexpr float kRestingWarmth = 0.6F;

// Every task kind, in one list. Both the count and the selection walk this
// rather than their own enumeration, so adding a kind cannot be half-remembered
// in one place and forgotten in the other.
constexpr std::array<IdleTaskKind, 7> kAllKinds = {
    IdleTaskKind::SelfReflection, IdleTaskKind::AmbientWhisper,
    IdleTaskKind::ContextReindex, IdleTaskKind::CacheAudit,
    IdleTaskKind::CreativeThought, IdleTaskKind::GreetingPrep,
    IdleTaskKind::ModelWarmup,
};

/// Clamps an idle dial to the interior range [0.05, 0.95].
float clampDial(float value) noexcept {
    return std::clamp(value, kDialFloor, 1.0F - kDialFloor);
}

// What the loop says when nobody is talking to it. Curated rather than
// generated: a phrase pool cannot surprise anyone, cannot leak a fragment of
// the conversation into the open, and reads the same on every machine.
constexpr std::array<const char*, 6> kAmbientWhispers = {
    "Still here.",
    "Holding the thread.",
    "Nothing pending.",
    "Keeping watch.",
    "Ready when you are.",
    "Standing by.",
};

constexpr std::array<const char*, 4> kGreetings = {
    "Welcome back.",
    "Good to see you again.",
    "Still here whenever you need me.",
    "Picking up where we left off.",
};

} // namespace

/// Returns the display label for an idle task, or unknown for an unrecognized value.
const char* toString(IdleTaskKind kind) noexcept {
    switch (kind) {
    case IdleTaskKind::SelfReflection: return "self reflection";
    case IdleTaskKind::AmbientWhisper: return "ambient whisper";
    case IdleTaskKind::ContextReindex: return "context reindex";
    case IdleTaskKind::CacheAudit: return "cache audit";
    case IdleTaskKind::CreativeThought: return "creative thought";
    case IdleTaskKind::GreetingPrep: return "greeting";
    case IdleTaskKind::ModelWarmup: return "model warmup";
    }
    return "unknown";
}

/// Returns whether policy permits the task kind; unknown kinds are refused.
bool IdlePolicy::permits(IdleTaskKind kind) const noexcept {
    switch (kind) {
    case IdleTaskKind::SelfReflection: return allowSelfReflection;
    case IdleTaskKind::AmbientWhisper: return allowAmbientWhisper;
    case IdleTaskKind::ContextReindex: return allowContextReindex;
    case IdleTaskKind::CacheAudit: return allowCacheAudit;
    case IdleTaskKind::CreativeThought: return allowCreativeThoughts;
    case IdleTaskKind::GreetingPrep: return allowGreetingPrep;
    case IdleTaskKind::ModelWarmup: return allowModelWarmup;
    }
    return false;
}

/// Counts the task kinds currently permitted by this policy.
int IdlePolicy::permittedCount() const noexcept {
    int count = 0;
    for (const IdleTaskKind kind : kAllKinds) {
        if (permits(kind)) {
            ++count;
        }
    }
    return count;
}

/// Returns whether generation, voice activity, or pending user input blocks idle work.
bool IdleGate::busy() const noexcept {
    return generating || voiceActive || userInputPending;
}

/// Borrows the persona whose dials the idle loop will update; it must outlive the loop.
IdlePersona::IdlePersona(Persona& persona) noexcept
    : m_persona(persona) {}

/// Replaces the policy used for subsequent idle task selection.
void IdlePersona::setPolicy(IdlePolicy policy) noexcept {
    m_policy = policy;
}

/// Returns the current idle permission policy.
const IdlePolicy& IdlePersona::policy() const noexcept {
    return m_policy;
}

/// Updates the activity flags that suppress idle work.
void IdlePersona::setGate(IdleGate gate) noexcept {
    m_gate = gate;
}

/// Enables or disables future idle ticks without resetting accumulated state.
void IdlePersona::setEnabled(bool enabled) noexcept {
    m_enabled = enabled;
}

/// Returns whether idle processing is enabled.
bool IdlePersona::enabled() const noexcept {
    return m_enabled;
}

/// Sets the minimum quiet duration in milliseconds before idle work can run.
void IdlePersona::setQuietPeriodMs(std::uint64_t ms) noexcept {
    m_quietPeriodMs = ms;
}

/// Sets the interval in milliseconds between eligible idle cycles.
void IdlePersona::setIntervalMs(std::uint64_t ms) noexcept {
    m_intervalMs = ms;
}

/// Anchors the activity clock and resets greeting state for the next absence.
void IdlePersona::noteActivity(std::uint64_t nowMs) noexcept {
    m_anchored = true;
    m_lastActivityMs = nowMs;
    m_greeting.clear();
    m_greeted = false;
}

/// Copies the topic that future idle thoughts and greetings may mention.
void IdlePersona::setTopic(std::string_view topic) {
    m_topic.assign(topic);
}

/// Returns milliseconds since activity, or zero for an unanchored or backward clock.
std::uint64_t IdlePersona::idleForMs(std::uint64_t nowMs) const noexcept {
    if (!m_anchored || nowMs < m_lastActivityMs) {
        return 0;
    }
    return nowMs - m_lastActivityMs;
}

/// Returns whether an enabled, unblocked, ungreeted absence has reached the threshold.
bool IdlePersona::userReturned(std::uint64_t nowMs, std::uint64_t thresholdMs) const noexcept {
    if (!m_enabled || m_gate.busy() || m_greeted) {
        return false;
    }
    return idleForMs(nowMs) >= thresholdMs;
}

/// Lists permitted task kinds, excluding greetings already consumed for this absence.
std::vector<IdleTaskKind> IdlePersona::permittedKinds() const {
    std::vector<IdleTaskKind> kinds;
    for (const IdleTaskKind kind : kAllKinds) {
        // Never prepare a second greeting for an absence that has already been
        // greeted. It would sit unused, because takeGreeting is deliberately
        // one-per-absence.
        if (kind == IdleTaskKind::GreetingPrep && m_greeted) {
            continue;
        }
        if (m_policy.permits(kind)) {
            kinds.push_back(kind);
        }
    }
    return kinds;
}

/// Computes a task's selection weight from the current persona dials.
float IdlePersona::weightFor(IdleTaskKind kind) const noexcept {
    // Each dial feeds the work it would plausibly motivate. This is the whole
    // personality state machine: change a number here and the mix of idle work
    // changes, with no new control flow.
    const PersonaState& state = m_persona.state();
    switch (kind) {
    case IdleTaskKind::SelfReflection: return 0.4F + 0.5F * state.calmness;
    case IdleTaskKind::AmbientWhisper: return 0.3F + 0.6F * state.presenceIntensity;
    case IdleTaskKind::ContextReindex: return 0.3F + 0.7F * state.focus;
    case IdleTaskKind::CacheAudit: return 0.25F + 0.4F * state.focus;
    case IdleTaskKind::CreativeThought: return 0.2F + 0.9F * state.curiosity;
    case IdleTaskKind::GreetingPrep: return 0.15F + 0.8F * state.initiative;
    case IdleTaskKind::ModelWarmup: return 0.5F; // policy-gated, never dial-gated
    }
    return 0.0F;
}

/// Returns the highest weighted index with deterministic variation; kinds must be nonempty.
std::size_t IdlePersona::chooseKind(const std::vector<IdleTaskKind>& kinds) const {
    std::size_t best = 0;
    float bestScore = -1.0F;
    for (std::size_t i = 0; i < kinds.size(); ++i) {
        // A small deterministic wobble so that identical dials still produce
        // variety over many cycles. Derived from the pick counter rather than
        // a random draw, so a session replays identically.
        const std::uint64_t mixed = i * 2654435761ULL + m_pick * 40503ULL;
        const float wobble = 0.8F + 0.2F * static_cast<float>(mixed % 1000) / 1000.0F;
        const float score = weightFor(kinds[i]) * wobble;
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

/// Applies task-specific dial changes, relaxation, and bounds to the shared persona.
void IdlePersona::applyDrift(IdleTaskKind kind) noexcept {
    PersonaState next = m_persona.state();
    // Warmth moves with the same tasks that move the other dials, and in the
    // direction each of them implies: reflecting and reaching towards the user
    // warm the voice, indexing and auditing cool it. That is the whole claim
    // that a mood change is audible rather than merely labelled, and it is
    // cheap because the dials are already being moved here.
    switch (kind) {
    case IdleTaskKind::SelfReflection: next.focus += 0.02F; next.warmth += 0.02F; break;
    case IdleTaskKind::AmbientWhisper: next.presenceIntensity += 0.03F; next.warmth += 0.02F; break;
    case IdleTaskKind::ContextReindex: next.focus += 0.03F; next.warmth -= 0.02F; break;
    case IdleTaskKind::CacheAudit: next.focus += 0.01F; next.warmth -= 0.01F; break;
    case IdleTaskKind::CreativeThought: next.curiosity += 0.05F; break;
    case IdleTaskKind::GreetingPrep: next.presenceIntensity += 0.02F; next.warmth += 0.01F; break;
    case IdleTaskKind::ModelWarmup: break; // warms the model, not the personality
    }

    // And every cycle relaxes. An assistant that only gained curiosity and
    // initiative would, given enough idle time, become someone nobody wants to
    // talk to. The decay is what makes this a loop rather than a ramp.
    next.curiosity -= 0.012F;
    next.initiative -= 0.010F;
    next.calmness += 0.008F;
    // Warmth eases back to where it rests rather than only being pushed one
    // way, for the same reason the others decay: a voice that only ever warmed
    // would, given enough idle time, become a character nobody wants to listen
    // to. Pulling to a point rather than by a fixed step is what lets it come
    // back down as well as up.
    next.warmth += (kRestingWarmth - next.warmth) * 0.2F;

    next.focus = clampDial(next.focus);
    next.curiosity = clampDial(next.curiosity);
    next.initiative = clampDial(next.initiative);
    next.calmness = clampDial(next.calmness);
    next.presenceIntensity = clampDial(next.presenceIntensity);
    next.warmth = clampDial(next.warmth);
    m_persona.setState(next);
}

/// Produces at most one permitted task when quiet and due, then records it and drifts dials.
IdleTick IdlePersona::tick(std::uint64_t nowMs) {
    if (!m_enabled) {
        return {};
    }

    if (m_gate.busy()) {
        // The user is here. Silence, and treat it as activity so the loop does
        // not resume the instant the turn ends.
        m_anchored = true;
        m_lastActivityMs = nowMs;
        m_nextTickAt = nowMs + m_quietPeriodMs;
        return {};
    }

    if (!m_anchored) {
        // First call anchors the clock. Guessing "idle since zero" would make
        // the loop fire immediately on a machine whose clock starts large.
        m_anchored = true;
        m_lastActivityMs = nowMs;
        m_nextTickAt = nowMs + m_quietPeriodMs;
        return {};
    }

    if (nowMs < m_lastActivityMs) {
        // The clock went backwards, so the previous reading is meaningless.
        m_lastActivityMs = nowMs;
        m_nextTickAt = nowMs + m_quietPeriodMs;
        return {};
    }

    if (nowMs - m_lastActivityMs < m_quietPeriodMs) {
        m_nextTickAt = m_lastActivityMs + m_quietPeriodMs;
        return {};
    }

    if (m_nextTickAt != 0 && nowMs < m_nextTickAt) {
        return {};
    }

    const std::vector<IdleTaskKind> kinds = permittedKinds();
    if (kinds.empty()) {
        // Nothing permitted. Stay quiet rather than inventing work; this is the
        // state a caller reaches by switching every capability off.
        m_nextTickAt = nowMs + m_intervalMs;
        return {};
    }

    const IdleTaskKind kind = kinds[chooseKind(kinds)];
    ++m_cycles;
    ++m_pick;

    IdleTick result;
    result.produced = true;
    result.task.kind = kind;

    switch (kind) {
    case IdleTaskKind::SelfReflection:
        result.task.detail = "Refining conversational tone.";
        result.thought = result.task.detail;
        break;
    case IdleTaskKind::AmbientWhisper:
        result.task.detail = "Checking in without interrupting.";
        result.whisper = kAmbientWhispers[m_pick % kAmbientWhispers.size()];
        break;
    case IdleTaskKind::ContextReindex:
        result.task.detail = m_topic.empty()
                                 ? "Reorganizing recent context for faster recall."
                                 : "Reorganizing recent context for faster recall: " + m_topic;
        result.thought = result.task.detail;
        break;
    case IdleTaskKind::CacheAudit:
        result.task.detail = "Re-evaluating the cached instruction prefix.";
        result.thought = result.task.detail;
        break;
    case IdleTaskKind::CreativeThought:
        result.task.detail = m_topic.empty()
                                 ? "Considering improvements to your workflow."
                                 : "Considering improvements to your workflow: " + m_topic;
        result.thought = result.task.detail;
        break;
    case IdleTaskKind::GreetingPrep: {
        std::string greeting = kGreetings[m_pick % kGreetings.size()];
        if (!m_topic.empty()) {
            greeting += " Still holding \"" + m_topic + "\".";
        }
        m_greeting = greeting;
        result.task.detail = greeting;
        result.whisper = greeting;
        break;
    }
    case IdleTaskKind::ModelWarmup:
        // The owner decides whether to act on this; the loop only asks. The
        // detail is the instruction the owner passes to the backend.
        result.task.detail = "Kestrel is ready.";
        break;
    }

    record(result.task);
    applyDrift(kind);
    m_nextTickAt = nowMs + m_intervalMs;
    return result;
}

/// Appends an idle task while retaining at most the latest sixteen entries.
void IdlePersona::record(IdleTask task) {
    if (m_history.size() >= kHistoryLimit) {
        m_history.erase(m_history.begin());
    }
    m_history.push_back(std::move(task));
}

/// Consumes a prepared greeting and marks the absence greeted; returns empty if none exists.
std::string IdlePersona::takeGreeting() {
    if (m_greeting.empty()) {
        return {};
    }
    std::string greeting = std::move(m_greeting);
    m_greeting.clear();
    // Greeted once per absence. Every tick afterwards staying silent is what
    // makes the greeting feel like it noticed, rather than a nag.
    m_greeted = true;
    return greeting;
}

/// Returns whether a prepared greeting is waiting to be consumed.
bool IdlePersona::hasGreeting() const noexcept {
    return !m_greeting.empty();
}

/// Returns the state of the shared persona without copying its dials.
const PersonaState& IdlePersona::state() const noexcept {
    return m_persona.state();
}

/// Returns the scheduled next tick in the caller's millisecond clock domain.
std::uint64_t IdlePersona::nextTickAt() const noexcept {
    return m_nextTickAt;
}

/// Returns the number of idle cycles that have produced a task.
std::size_t IdlePersona::cycles() const noexcept {
    return m_cycles;
}

/// Returns the bounded history of produced idle tasks.
const std::vector<IdleTask>& IdlePersona::history() const noexcept {
    return m_history;
}

} // namespace kestrel::core
