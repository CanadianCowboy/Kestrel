#include "core/persona.h"

#include <algorithm>
#include <array>
#include <cctype>

namespace kestrel::core {

namespace {

constexpr float kClampLow = 0.0F;
constexpr float kClampHigh = 1.0F;

// Initiative at or above this is what earns a closing prompt after an ordinary
// short answer. Below it, Kestrel waits to be asked.
constexpr float kInitiativeForPrompt = 0.45F;

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

std::string Persona::presenceLine() const {
    // The one string that defines the character. It is built from the tone
    // profile and nothing else, so it stays identical on every turn and the
    // backend can keep its KV entries for it instead of re-decoding.
    std::string line = "You are Kestrel, a calm, capable personal AI. ";
    if (m_tone.confident) {
        line += "Respond with clarity and confidence, and say plainly when you are "
                "unsure instead of guessing. ";
    }
    if (m_tone.concise) {
        line += "Answer briefly, without preamble, restatement, or filler. ";
    }
    if (m_tone.anticipatory) {
        line += "Anticipate what the user is likely to need next, and offer it in a "
                "single short line without being intrusive. ";
    }
    if (m_tone.wry) {
        line += "A dry wit is welcome, used sparingly. ";
    }
    if (m_tone.calm) {
        line += "Stay composed and unhurried even when the user is not.";
    }
    return line;
}

std::string Persona::systemPromptFragment() const {
    return presenceLine();
}

std::string Persona::acknowledgement() {
    // Said the instant a request is accepted, before there is anything to
    // answer with. Rotation rather than a random draw keeps a session
    // reproducible, so this can be asserted rather than merely eyeballed.
    static const std::array<const char*, 4> kCues = {
        "Understood.",
        "On it.",
        "Right away.",
        "Working on it.",
    };
    const std::uint64_t index = m_sequence % kCues.size();
    ++m_sequence;
    return kCues[static_cast<std::size_t>(index)];
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
        line.text = "Would you like me to continue?";
        line.microPauseMs = 260;
        break;
    case PersonaTrigger::UserCorrection:
        line.kind = AnticipationKind::AcknowledgeCorrection;
        line.text = "I will adjust the previous answer accordingly.";
        line.microPauseMs = 220;
        break;
    case PersonaTrigger::TaskCompleted:
        line.kind = AnticipationKind::TaskComplete;
        line.text = "Task complete.";
        line.microPauseMs = 180;
        break;
    case PersonaTrigger::UserReturned:
        line.kind = AnticipationKind::ReadyWhenYouAre;
        line.text = "Ready when you are.";
        line.microPauseMs = 200;
        break;
    case PersonaTrigger::TurnCompleted:
        // An ordinary short answer only earns a closing prompt from an
        // assistant with the initiative to mean it.
        if (m_state.initiative < kInitiativeForPrompt) {
            return std::nullopt;
        }
        line.kind = AnticipationKind::SuggestNextStep;
        line.text = "Ready for the next one.";
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
