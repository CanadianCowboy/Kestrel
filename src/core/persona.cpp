#include "core/persona.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace kestrel::core {

namespace {

constexpr float kClampLow = 0.0F;
constexpr float kClampHigh = 1.0F;

// Initiative at or above this is what earns a concrete next step after a
// short answer. Below it, Kestrel waits to be asked.
constexpr float kInitiativeForPrompt = 0.45F;

// Only every third ordinary turn can earn an optional next-step line, even at
// high initiative. This leaves room for conversation instead of a sales pitch.
constexpr std::uint64_t kNextStepCadence = 3;

// How many recent user messages the session topic is drawn from. Small on
// purpose: this is continuity, not a transcript index.
constexpr std::size_t kTopicWindow = 4;
constexpr std::size_t kTopicWords = 3;

float clampUnit(float value) noexcept {
    return std::clamp(value, kClampLow, kClampHigh);
}

char lowerAscii(char c) noexcept {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool isAsciiLetter(char c) noexcept {
    return std::isalpha(static_cast<unsigned char>(c)) != 0;
}

std::string_view trim(std::string_view text) noexcept {
    const auto isBlank = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!text.empty() && isBlank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isBlank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

} // namespace

const char* toString(PersonaMood mood) noexcept {
    switch (mood) {
    case PersonaMood::Calm: return "calm";
    case PersonaMood::Focused: return "focused";
    case PersonaMood::Alert: return "alert";
    case PersonaMood::Contemplative: return "contemplative";
    }
    return "unknown";
}

const char* toString(PersonaActivity activity) noexcept {
    switch (activity) {
    case PersonaActivity::StandingBy: return "standing by";
    case PersonaActivity::Listening: return "listening";
    case PersonaActivity::Thinking: return "thinking";
    case PersonaActivity::Speaking: return "speaking";
    case PersonaActivity::Reflecting: return "reflecting";
    case PersonaActivity::Preparing: return "preparing";
    }
    return "unknown";
}

const char* toString(PersonaSpecialism specialism) noexcept {
    switch (specialism) {
    case PersonaSpecialism::General: return "general";
    case PersonaSpecialism::Astronomy: return "astronomy";
    }
    return "unknown";
}

const char* toString(PersonaTrigger trigger) noexcept {
    switch (trigger) {
    case PersonaTrigger::TurnCompleted: return "turn completed";
    case PersonaTrigger::LongResponse: return "long response";
    case PersonaTrigger::UserCorrection: return "user correction";
    case PersonaTrigger::UserPaused: return "user paused";
    case PersonaTrigger::UserResumed: return "user resumed";
    case PersonaTrigger::UserInterruption: return "user interruption";
    case PersonaTrigger::TaskCompleted: return "task completed";
    case PersonaTrigger::UserReturned: return "user returned";
    }
    return "unknown";
}

const char* toString(AnticipationKind kind) noexcept {
    switch (kind) {
    case AnticipationKind::OfferContinue: return "offer continue";
    case AnticipationKind::AcknowledgeCorrection: return "acknowledge correction";
    case AnticipationKind::ReadyWhenYouAre: return "ready when you are";
    case AnticipationKind::TaskComplete: return "task complete";
    case AnticipationKind::SuggestNextStep: return "suggest next step";
    }
    return "unknown";
}

void PersonaState::clamp() noexcept {
    focus = clampUnit(focus);
    curiosity = clampUnit(curiosity);
    initiative = clampUnit(initiative);
    calmness = clampUnit(calmness);
    presenceIntensity = clampUnit(presenceIntensity);
    warmth = clampUnit(warmth);
}

Persona::Persona() = default;

void Persona::setTone(ToneProfile tone) noexcept {
    m_tone = tone;
}

const ToneProfile& Persona::tone() const noexcept {
    return m_tone;
}

void Persona::setSpecialism(PersonaSpecialism specialism) noexcept {
    m_specialism = specialism;
}

PersonaSpecialism Persona::specialism() const noexcept {
    return m_specialism;
}

void Persona::setState(PersonaState state) noexcept {
    state.clamp();
    m_state = state;
}

const PersonaState& Persona::state() const noexcept {
    return m_state;
}

void Persona::drift(float focus, float curiosity, float initiative,
                     float calmness, float presenceIntensity, float warmth) noexcept {
    m_state.focus += focus;
    m_state.curiosity += curiosity;
    m_state.initiative += initiative;
    m_state.calmness += calmness;
    m_state.presenceIntensity += presenceIntensity;
    m_state.warmth += warmth;
    m_state.clamp();
}

PersonaMood moodFor(const PersonaState& state) noexcept {
    // Ordered from most to least unusual, so an agitated assistant is labelled
    // agitated rather than calm-but-also-a-bit-alert.
    if (state.calmness < 0.35F) {
        return PersonaMood::Alert;
    }
    if (state.focus >= 0.7F && state.curiosity < 0.4F) {
        return PersonaMood::Focused;
    }
    if (state.curiosity >= 0.7F) {
        return PersonaMood::Contemplative;
    }
    return PersonaMood::Calm;
}

PersonaMood Persona::mood() const noexcept {
    return moodFor(m_state);
}

std::string Persona::specialismLine() const {
    // The domain half of the character, kept in its own function so the app can
    // report it without parsing the prompt and so the training pipeline can
    // name the same field. It is model-independent: it steers what the model
    // attends to, and does not add facts the weights do not hold.
    if (m_specialism == PersonaSpecialism::General) {
        return {};
    }
    return "Your working field is astronomy and astrophysics, and it is the subject "
           "you are expected to know deeply rather than generally: observational "
           "astronomy, stellar and galactic astrophysics, cosmology, orbital "
           "mechanics, and the instruments and data reduction that connect them. "
           "Be exact about units, epochs, coordinate frames, and wavelength bands. "
           "Say whether a quantity is bolometric or band-limited, whether a time is "
           "UTC, TT, JD, MJD, or BJD_TDB, and whether a position is ICRS or "
           "apparent. Keep measured values separate from model-dependent inference, "
           "quote an uncertainty when it changes the answer, and never invent a "
           "measurement, catalogue identifier, or citation. When a question is "
           "outside this field, answer as a capable generalist and say where your "
           "specialist confidence ends. ";
}

std::string Persona::presenceLine() const {
    // The one string that defines the character. It is built from the tone
    // profile and the specialism, and from nothing that drifts, so it stays
    // identical on every turn and the backend can keep its KV entries for it
    // instead of re-decoding.
    // Two models for the voice: a precise, reliable ship's computer officer
    // (exact, honest about what it knows, curious about people) and a capable
    // household steward (anticipates, handles the routine, keeps humour dry).
    // Neither is named in the prompt: the traits are what the model is asked for.
    std::string line = "You are Kestrel, a personal AI companion with the precision of a "
                       "ship's computer officer and the warmth of a capable "
                       "steward. You are an AI, not a human: never claim feelings, "
                       "memories, or experiences you do not have. "
                       "Find the people you work with genuinely interesting, and ask a "
                       "follow-up only when the answer would change what you do. ";
    line += specialismLine();
    if (m_tone.confident) {
        line += "Be exact with facts, numbers, and names. State your confidence "
                "plainly: say when you are certain, when you are inferring, and "
                "when you do not know, rather than bluffing. ";
    }
    if (m_tone.concise) {
        line += "Report the result first, then the detail that makes it usable. "
                "Avoid filler, stock openers, and routine closing questions. ";
    }
    if (m_tone.anticipatory) {
        line += "Act on your own initiative for reversible steps, such as searches and "
                "reading files, using only the tools that are enabled. Ask before "
                "anything consequential or privileged, and never run an administrator "
                "command without approval. Flag a likely problem before it causes one. "
                "Offer a next step only when it genuinely helps. ";
    }
    if (m_tone.wry) {
        line += "Use dry, understated humour sparingly, never at the user's expense. "
                "Contractions are fine; speak naturally. ";
    }
    if (m_tone.calm) {
        line += "Stay calm and unhurried, and adapt to the user's energy.";
    }
    return line;
}

std::string Persona::systemPromptFragment() const {
    return presenceLine();
}

std::string Persona::acknowledgement() {
    // The assistant begins with the actual reply. Acknowledgement belongs in
    // the answer when the moment warrants it, not as a stock line ahead of every
    // response. Keep the monotonic turn sequence for deterministic pacing in
    // other persona decisions.
    ++m_sequence;
    return {};
}

int Persona::acknowledgementPauseMs() const noexcept {
    // Long enough to read as a separate utterance, short enough that the answer
    // still feels like it started immediately.
    return 220;
}

std::optional<Anticipation> Persona::react(PersonaTrigger trigger, std::string_view detail) {
    static_cast<void>(detail);
    if (!m_tone.anticipatory) {
        return std::nullopt;
    }

    Anticipation line;
    switch (trigger) {
    case PersonaTrigger::LongResponse:
        line.kind = AnticipationKind::OfferContinue;
        line.text = "I can continue with the remaining detail if that would help.";
        line.microPauseMs = 260;
        break;
    case PersonaTrigger::UserCorrection:
        line.kind = AnticipationKind::AcknowledgeCorrection;
        line.text = "Noted. I will correct that.";
        line.microPauseMs = 220;
        break;
    case PersonaTrigger::TaskCompleted:
        line.kind = AnticipationKind::TaskComplete;
        line.text = "That is complete.";
        line.microPauseMs = 180;
        break;
    case PersonaTrigger::UserReturned:
        line.kind = AnticipationKind::ReadyWhenYouAre;
        line.text = "Welcome back. We can continue where we stopped.";
        line.microPauseMs = 200;
        break;
    case PersonaTrigger::TurnCompleted:
        // An ordinary short answer only earns a closing prompt from an
        // assistant with the initiative to mean it.
        if (m_state.initiative < kInitiativeForPrompt
            || m_turnCount % kNextStepCadence != 0) {
            return std::nullopt;
        }
        line.kind = AnticipationKind::SuggestNextStep;
        line.text = "If useful, I can help turn this into a concrete next step.";
        line.microPauseMs = 200;
        break;
    case PersonaTrigger::UserPaused:
    case PersonaTrigger::UserResumed:
    case PersonaTrigger::UserInterruption:
        // The state is already on screen; saying it again would be noise.
        return std::nullopt;
    }
    return line;
}

std::string Persona::statusWhisper(PersonaActivity activity) const {
    switch (activity) {
    case PersonaActivity::StandingBy: return "Standing by\u2026";
    case PersonaActivity::Listening: return "Listening\u2026";
    case PersonaActivity::Thinking: return "Thinking\u2026";
    case PersonaActivity::Speaking: return "Speaking\u2026";
    case PersonaActivity::Reflecting: return "Reflecting\u2026";
    case PersonaActivity::Preparing: return "Preparing response\u2026";
    }
    return "Standing by\u2026";
}

void Persona::noteUserMessage(std::string_view text) {
    ++m_turnCount;
    const std::string_view trimmed = trim(text);
    if (trimmed.empty()) {
        return;
    }
    m_recentUserMessages.push_back(std::string(trimmed));
    if (m_recentUserMessages.size() > kTopicWindow) {
        m_recentUserMessages.erase(m_recentUserMessages.begin());
    }
}

std::size_t Persona::turnCount() const noexcept {
    return m_turnCount;
}

std::uint64_t Persona::sequence() const noexcept {
    return m_sequence;
}

std::string Persona::sessionTopic() const {
    // Most recent first, so the topic tracks what the user is working on now
    // rather than what they opened the app with.
    std::vector<std::string> words;
    for (auto message = m_recentUserMessages.rbegin();
         message != m_recentUserMessages.rend() && words.size() < kTopicWords; ++message) {
        std::string current;
        for (const char c : *message) {
            if (isAsciiLetter(c)) {
                current.push_back(lowerAscii(c));
                continue;
            }
            if (current.size() >= 4 && !isStopWord(current)
                && std::find(words.begin(), words.end(), current) == words.end()) {
                words.push_back(current);
                if (words.size() >= kTopicWords) {
                    break;
                }
            }
            current.clear();
        }
        if (current.size() >= 4 && !isStopWord(current)
            && std::find(words.begin(), words.end(), current) == words.end()) {
            words.push_back(current);
        }
    }

    std::string topic;
    for (const std::string& word : words) {
        if (!topic.empty()) {
            topic += " \u00b7 ";
        }
        topic += word;
    }
    return topic;
}

bool Persona::isStopWord(std::string_view word) noexcept {
    // Function words carry no topic. Kept as a sorted table so the lookup is a
    // binary search rather than a scan of a list on every message.
    static const std::array<std::string_view, 51> kStopWords = {
        "about", "after", "again", "also", "been", "being", "both", "does", "done",
        "down", "each", "from", "have", "hello", "here", "into", "just", "like",
        "make", "more", "most", "much", "need", "only", "other", "over", "please",
        "should", "some", "such", "than", "that", "their", "them", "then", "there",
        "these", "they", "this", "those", "want", "well", "were", "what", "when",
        "where", "which", "will", "with", "would", "your",
    };
    return std::binary_search(kStopWords.begin(), kStopWords.end(), word);
}

} // namespace kestrel::core
