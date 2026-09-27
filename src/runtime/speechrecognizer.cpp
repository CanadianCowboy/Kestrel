#include "runtime/speechrecognizer.h"

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

} // namespace kestrel::runtime
