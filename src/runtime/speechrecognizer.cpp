#include "runtime/speechrecognizer.h"

#include "runtime/sapirecognizer.h"

#include <algorithm>
#include <sstream>
#include <utility>

namespace kestrel::runtime {

namespace {

// How many words of a phrase each partial result carries. Real engines report
// partials far more often than this, but a consumer only needs to see them
// grow, and a test that has to wait for forty of them is a slow test.
constexpr std::size_t kWordsPerPartial = 2;

} // namespace

const char* toString(RecognitionEnd reason) noexcept {
    switch (reason) {
    case RecognitionEnd::Silence: return "silence";
    case RecognitionEnd::Cancelled: return "cancelled";
    case RecognitionEnd::NoAudio: return "no audio";
    case RecognitionEnd::Failed: return "failed";
    }
    return "unknown";
}

MockSpeechRecognizer::MockSpeechRecognizer(std::vector<std::string> phrases)
    : m_phrases(std::move(phrases)) {
    if (m_phrases.empty()) {
        m_phrases.emplace_back("What is the plan for tensorrt?");
    }
}

std::string MockSpeechRecognizer::detail() const {
    return "preview recognizer (no microphone)";
}

bool MockSpeechRecognizer::start(ResultCallback onResult, EndCallback onEnd, std::string& error) {
    if (m_listening) {
        error = "already listening";
        return false;
    }
    if (!onResult || !onEnd) {
        error = "start needs both a result and an end callback";
        return false;
    }
    m_onResult = std::move(onResult);
    m_onEnd = std::move(onEnd);
    m_listening = true;
    m_phrase = 0;
    m_word = 0;
    return true;
}

void MockSpeechRecognizer::stop() {
    if (!m_listening) {
        return;
    }
    m_listening = false;
    // Copied before the callbacks run and cleared first. A callback is allowed
    // to stop the recognizer -- that is exactly what happens when a recognized
    // phrase starts a turn and the session tears the microphone down -- so the
    // member callbacks can be emptied by the first one while a second is still
    // to be delivered. Calling through a copy that outlives the call stack is
    // what keeps that from becoming a call on an empty std::function.
    ResultCallback onResult = std::move(m_onResult);
    EndCallback onEnd = std::move(m_onEnd);
    m_onResult = {};
    m_onEnd = {};

    if (!onResult || !onEnd) {
        return;
    }
    // A stop is a cancellation, not a completed phrase. Reporting a final
    // result here would hand the user a half-sentence as if they had said it,
    // which is the single worst thing a recognizer can do.
    RecognitionResult cancelled;
    cancelled.isFinal = false;
    cancelled.end = RecognitionEnd::Cancelled;
    onResult(cancelled);
    onEnd(RecognitionEnd::Cancelled, {});
}

void MockSpeechRecognizer::emitNextPartial() {
    if (!m_listening || m_phrase >= m_phrases.size()) {
        return;
    }
    // See stop(): a callback may stop this recognizer, so the callbacks are
    // copied before use and never read back off the member afterwards.
    const ResultCallback onResult = m_onResult;
    const EndCallback onEnd = m_onEnd;
    if (!onResult || !onEnd) {
        return;
    }

    // Split the phrase once and hand back the next slice of it. Real engines
    // report the whole phrase so far on every partial; so does this, which is
    // what lets a consumer simply overwrite what it shows.
    std::vector<std::string> words;
    {
        std::istringstream stream(m_phrases[m_phrase]);
        std::string word;
        while (stream >> word) {
            words.push_back(word);
        }
    }
    if (words.empty()) {
        return;
    }

    const std::size_t take = std::min(words.size(), m_word + kWordsPerPartial);
    std::string text;
    for (std::size_t i = 0; i < take; ++i) {
        if (!text.empty()) {
            text += ' ';
        }
        text += words[i];
    }
    m_word = take;

    if (take < words.size()) {
        RecognitionResult partial;
        partial.text = text;
        partial.confidence = 0.5;
        partial.isFinal = false;
        onResult(partial);
        return;
    }

    RecognitionResult final;
    final.text = text;
    final.confidence = 0.9;
    final.isFinal = true;
    final.end = RecognitionEnd::Silence;
    onResult(final);
    onEnd(RecognitionEnd::Silence, {});

    ++m_phrase;
    m_word = 0;
    if (m_phrase >= m_phrases.size()) {
        m_listening = false;
    }
}

bool preferPlatformRecognizer(Microphone microphone) noexcept {
    // One rule, and it is the obvious one: a real engine is for a machine that
    // has a microphone to feed it. Everywhere else the mock is not a
    // placeholder, it is the only thing that can work, because it is the only
    // recognizer that does not need audio hardware to produce a phrase.
    return microphone == Microphone::Present;
}

std::unique_ptr<SpeechRecognizer> makeRecognizerFor(Microphone microphone) {
    if (preferPlatformRecognizer(microphone)) {
        // The probe counts devices, which is not the same as being able to
        // listen: a machine can advertise a capture endpoint that refuses to
        // open, and a build can have no recognizer at all. A platform adapter
        // that says it cannot listen is an honest answer, and the mock is the
        // honest fallback behind it -- so the user gets a working microphone
        // button and a detail() line that says which of the two they have.
        if (auto platform = makePlatformSpeechRecognizer();
            platform != nullptr && platform->available()) {
            return platform;
        }
    }
    return std::make_unique<MockSpeechRecognizer>();
}

std::unique_ptr<SpeechRecognizer> makeBestSpeechRecognizer(SpeechInput preference) {
    // The override exists for two callers that would otherwise be at the mercy
    // of the machine. The test suite asserts on partial results, which only the
    // mock produces, so it must not change because someone plugged in a
    // microphone. And a user debugging dictation needs to be able to ask for
    // the real adapter explicitly, including on a machine where the probe says
    // there is no input device, to be shown the error rather than the mock.
    switch (preference) {
    case SpeechInput::Mock:
        return std::make_unique<MockSpeechRecognizer>();
    case SpeechInput::Platform:
        // Asked for by name, so the name is what is given. A recognizer that
        // cannot listen reports it through available() and detail(), and the UI
        // says so; quietly handing back the mock here would make a broken
        // microphone look like a working one.
        return makePlatformSpeechRecognizer();
    case SpeechInput::Auto:
        break;
    }
    return makeRecognizerFor(probeMicrophone());
}

} // namespace kestrel::runtime
