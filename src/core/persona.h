#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::core {

// How Kestrel feels, as a coarse label. The idle loop picks work by mood and
// the UI shows it, so both sides read the same vocabulary instead of each
// inventing thresholds over the same numbers.
enum class PersonaMood {
    Calm,
    Focused,
    Alert,
    Contemplative,
};

// What Kestrel is doing right now. Kept apart from mood on purpose: a persona
// can be calm while it is thinking, and animating on "calm" would be a bug.
enum class PersonaActivity {
    StandingBy,
    Listening,
    Thinking,
    Speaking,
    Reflecting,
    Preparing,
};

[[nodiscard]] const char* toString(PersonaMood mood) noexcept;
[[nodiscard]] const char* toString(PersonaActivity activity) noexcept;

// The five dials behind the mood label. Every one stays inside [0, 1].
//
// The idle loop drifts these and then weights its own work by them, which is
// what turns "personality" from a paragraph of prompt text into arithmetic:
// changing a dial changes behaviour without changing a line of control flow.
struct PersonaState {
    float focus = 0.55F;
    float curiosity = 0.5F;
    float initiative = 0.35F;
    float calmness = 0.8F;
    float presenceIntensity = 0.4F;

    void clamp() noexcept;
};

// The label for a set of dials. Free function rather than a Persona member so
// that every layer reading a PersonaState -- the persona, the presence engine,
// the idle loop -- resolves it the same way instead of copying thresholds.
[[nodiscard]] PersonaMood moodFor(const PersonaState& state) noexcept;

// Behavioural rules for the session.
//
// Deliberately not drifting. These decide the assistant-presence line that goes
// into the shared system prompt, and a prompt that changed between turns would
// throw away the backend's cached prefix on every turn. The dials move; the
// rules do not.
struct ToneProfile {
    bool calm = true;
    bool confident = true;
    bool concise = true;
    bool anticipatory = true;
    // "Slightly witty". Off by default because a wry assistant that nobody
    // asked for is a worse default than a plain one.
    bool wry = false;
};

// Events the persona reacts to. These are meanings, not UI events: the app
// decides what happened, the persona decides what, if anything, to say.
enum class PersonaTrigger {
    TurnCompleted,
    LongResponse,
    UserCorrection,
    UserPaused,
    UserResumed,
    UserInterruption,
    TaskCompleted,
    UserReturned,
};

[[nodiscard]] const char* toString(PersonaTrigger trigger) noexcept;

enum class AnticipationKind {
    OfferContinue,
    AcknowledgeCorrection,
    ReadyWhenYouAre,
    TaskComplete,
    SuggestNextStep,
};

[[nodiscard]] const char* toString(AnticipationKind kind) noexcept;

// One anticipatory line, ready for the UI or the voice.
struct Anticipation {
    AnticipationKind kind = AnticipationKind::OfferContinue;
    std::string text;
    // The pause that follows the line, so it does not butt up against whatever
    // is said next.
    int microPauseMs = 180;
};

// The personality layer.
//
// It lives in core, beside Conversation, because a persona is domain policy
// about how Kestrel answers rather than anything about voices, Qt, or the
// model runtime. It generates no tokens of its own: it produces the short
// lines the app places around a turn, plus the presence line that goes into the
// shared system prompt.
//
// Every choice is deterministic. Rotation is driven by an internal sequence
// number rather than a random draw, so a session replays identically and the
// behaviour can be asserted in tests.
class Persona {
public:
    Persona();

    void setTone(ToneProfile tone) noexcept;
    [[nodiscard]] const ToneProfile& tone() const noexcept;

    void setState(PersonaState state) noexcept;
    [[nodiscard]] const PersonaState& state() const noexcept;
    /// Moves each dial by the given amount and clamps back into [0, 1]. This
    /// is the only way dials change, so a caller cannot put one out of range.
    void drift(float focus, float curiosity, float initiative,
               float calmness, float presenceIntensity) noexcept;
    [[nodiscard]] PersonaMood mood() const noexcept;

    // The assistant-presence string injected into the shared system prompt.
    // Built from the tone profile alone, never from the drifting dials, so it
    // is byte-identical on every turn.
    [[nodiscard]] std::string presenceLine() const;
    /// presenceLine() plus the operating rules, as one prompt fragment.
    [[nodiscard]] std::string systemPromptFragment() const;

    // The short cue said the moment a request is accepted, before any of the
    // answer exists: "Understood." / "On it." / "Right away." Advances the
    // rotation, so consecutive requests do not repeat the same cue.
    [[nodiscard]] std::string acknowledgement();
    /// The pause the voice should take after the cue, from the same profile
    // that paces clauses, so the gap between cue and answer is deliberate.
    [[nodiscard]] int acknowledgementPauseMs() const noexcept;

    // Anticipatory micro-behaviour. Returns nothing when the tone profile has
    // anticipation switched off, when the trigger does not warrant a line, or
    // when the dials are not in a state that justifies saying it.
    //
    // The rules, in one place so they are easy to argue with:
    //   * a long answer is offered to continue,
    //   * a correction is acknowledged rather than argued with,
    //   * a finished task is reported as finished,
    //   * a returning user is met rather than interrogated,
    //   * an ordinary short answer only prompts the next step when initiative
    //     is high enough to be worth the interruption,
    //   * pausing, resuming, and barge-in say nothing, because the UI already
    //     shows those states and a line would be noise on top of them.
    [[nodiscard]] std::optional<Anticipation> react(PersonaTrigger trigger,
                                                    std::string_view detail = {});

    // The single status line the UI shows under the composer.
    [[nodiscard]] std::string statusWhisper(PersonaActivity activity) const;

    // Per-session continuity. This is not memory storage: nothing is written
    // anywhere and it dies with the process. It exists so the idle loop and the
    // return greeting have something concrete to refer to instead of inventing
    // context.
    void noteUserMessage(std::string_view text);
    [[nodiscard]] std::size_t turnCount() const noexcept;
    /// Up to three distinctive words from recent user messages, most recent
    /// first. Empty when nothing distinctive has been said.
    [[nodiscard]] std::string sessionTopic() const;
    /// Monotonic count of choices made, for callers that want to vary their
    /// own rotation in step with this one.
    [[nodiscard]] std::uint64_t sequence() const noexcept;

private:
    [[nodiscard]] static bool isStopWord(std::string_view word) noexcept;

    ToneProfile m_tone;
    PersonaState m_state;
    std::vector<std::string> m_recentUserMessages;
    std::size_t m_turnCount = 0;
    std::uint64_t m_sequence = 0;
};

} // namespace kestrel::core
