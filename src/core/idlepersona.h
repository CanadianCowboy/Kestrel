#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/persona.h"

namespace kestrel::core {

// The kinds of thing Kestrel can do when nobody is talking to it.
//
// The set is closed and deliberately small. Every entry is local, reversible,
// and either a string or a number; there is no "run a command" or "call an
// API" member here, because a loop that can be handed arbitrary work stops
// being a screensaver and starts being an unattended agent.
enum class IdleTaskKind {
    // "Refining conversational tone." Pure bookkeeping, no output.
    SelfReflection,
    // A short line for the status area, e.g. "Standing by…".
    AmbientWhisper,
    // "Reorganizing recent context for faster recall." Touches nothing outside
    // the process; the effect is on Kestrel's own topic summary.
    ContextReindex,
    // Re-evaluating cached prefix and KV accounting the owner exposes.
    CacheAudit,
    // "Considering improvements to your workflow." Private by default.
    CreativeThought,
    // Preparing something to say when the user comes back.
    GreetingPrep,
    // Runs a tiny generation to keep the GPU warm. The only task that touches
    // the model runtime.
    ModelWarmup,
};

[[nodiscard]] const char* toString(IdleTaskKind kind) noexcept;

// The safety boundary, stated as data.
//
// There is deliberately no permission here for the network or the filesystem,
// because IdlePersona has no way to reach either: it produces strings, numbers,
// and an enum. The only capability that leaves pure computation is GPU
// prewarming, and it is bounded -- a fixed eight-token prompt through the same
// worker as a real request -- rather than switched off, so a build that never
// touches a model behaves identically to one that does.
struct IdlePolicy {
    bool allowSelfReflection = true;
    bool allowAmbientWhisper = true;
    bool allowContextReindex = true;
    bool allowCacheAudit = true;
    bool allowCreativeThoughts = true;
    bool allowGreetingPrep = true;
    // On by default, and it used to be off. The reason it was off is that
    // warmup is the one idle task that leaves pure computation: every other
    // task produces a string the user might read, while this runs a tiny
    // generation and throws the tokens away. That is a real difference, and it
    // was treated as a capability to gate.
    //
    // The cost of gating it was that the feature simply never happened. A
    // default of false means the GPU is cold for the first reply of every
    // session, which is the entire problem warmup exists to remove -- so the
    // "warming up" the user could observe was a policy that guaranteed no
    // warming happened, reported as though it were running.
    //
    // It is bounded rather than open-ended: eight tokens, a fixed prompt, and
    // the same worker and cancellation path as a real request, so it competes
    // with nothing and is abandoned the moment the user types. Turning it on
    // costs a few milliseconds of idle work and removes a cold start.
    bool allowModelWarmup = true;

    [[nodiscard]] bool permits(IdleTaskKind kind) const noexcept;
    [[nodiscard]] int permittedCount() const noexcept;
};

// Silence while the user is present. While this is true the loop produces
// nothing at all: no thought, no whisper, no drift.
struct IdleGate {
    bool generating = false;
    bool voiceActive = false;
    // Something is typed but not yet sent, or a response is still being read.
    bool userInputPending = false;

    [[nodiscard]] bool busy() const noexcept;
};

struct IdleTask {
    IdleTaskKind kind = IdleTaskKind::SelfReflection;
    std::string detail;
};

// The result of one thought cycle.
struct IdleTick {
    bool produced = false;
    IdleTask task;
    // Set for AmbientWhisper and GreetingPrep: the line the UI may show.
    std::string whisper;
    // Set for SelfReflection and CreativeThought: internal, and surfaced only
    // when the owner explicitly reveals thoughts.
    std::string thought;
};

// Kestrel's private loop, for the time between requests.
//
// The contract with the owner is narrow on purpose: call tick() with a
// monotonic millisecond reading, and the loop decides whether this moment is
// one where it should do something. It is deterministic, so a session replays
// identically and its behaviour can be asserted in tests.
class IdlePersona {
public:
    // Holds a reference to the persona whose dials it drifts. The loop does not
    // own a second copy of them: one set of numbers, two readers, is the only
    // way the UI and the loop can ever agree on what Kestrel is like.
    explicit IdlePersona(Persona& persona) noexcept;

    void setPolicy(IdlePolicy policy) noexcept;
    [[nodiscard]] const IdlePolicy& policy() const noexcept;
    void setGate(IdleGate gate) noexcept;
    void setEnabled(bool enabled) noexcept;
    [[nodiscard]] bool enabled() const noexcept;

    // How long the user must be gone before the loop starts, and how often it
    // then runs. Both are settable because "never idle" and "never chatty" are
    // different goals and the right numbers depend on the machine.
    void setQuietPeriodMs(std::uint64_t ms) noexcept;
    void setIntervalMs(std::uint64_t ms) noexcept;

    // Any user or assistant action, stamped with the caller's current reading.
    // The time is a parameter rather than read internally so the loop has no
    // clock of its own, which is what keeps it testable.
    void noteActivity(std::uint64_t nowMs) noexcept;
    [[nodiscard]] std::uint64_t idleForMs(std::uint64_t nowMs) const noexcept;

    // The subject the loop may mention when it reorganizes context or muses
    // about the workflow. Supplied by the owner from Persona::sessionTopic();
    // the loop never reaches back into the persona itself.
    void setTopic(std::string_view topic);

    // True when the user has been gone long enough to be worth greeting, and
    // nothing has been said since. The owner decides what counts as "long".
    [[nodiscard]] bool userReturned(std::uint64_t nowMs, std::uint64_t thresholdMs) const noexcept;

    // One thought cycle. Returns an empty tick when it is not this loop's turn
    // to act, which is the common case.
    [[nodiscard]] IdleTick tick(std::uint64_t nowMs);
    [[nodiscard]] std::uint64_t nextTickAt() const noexcept;

    // The prepared return greeting, consumed on read so a long absence is
    // greeted once rather than on every tick.
    [[nodiscard]] std::string takeGreeting();
    [[nodiscard]] bool hasGreeting() const noexcept;

    [[nodiscard]] const PersonaState& state() const noexcept;
    [[nodiscard]] std::size_t cycles() const noexcept;
    // Bounded history of what it has been doing, for the diagnostics panel.
    [[nodiscard]] const std::vector<IdleTask>& history() const noexcept;

private:
    [[nodiscard]] std::vector<IdleTaskKind> permittedKinds() const;
    [[nodiscard]] std::size_t chooseKind(const std::vector<IdleTaskKind>& kinds) const;
    [[nodiscard]] float weightFor(IdleTaskKind kind) const noexcept;
    void applyDrift(IdleTaskKind kind) noexcept;
    void record(IdleTask task);

    Persona& m_persona;
    IdlePolicy m_policy;
    IdleGate m_gate;
    bool m_enabled = true;
    // Set once the owner has supplied a real clock reading, so the first tick
    // anchors to it instead of assuming the process started at zero.
    bool m_anchored = false;
    bool m_greeted = false;
    std::uint64_t m_quietPeriodMs = 6000;
    std::uint64_t m_intervalMs = 9000;
    std::uint64_t m_lastActivityMs = 0;
    std::uint64_t m_nextTickAt = 0;
    std::uint64_t m_pick = 0;
    std::size_t m_cycles = 0;
    std::string m_greeting;
    std::vector<IdleTask> m_history;
    std::string m_topic;
};

} // namespace kestrel::core
