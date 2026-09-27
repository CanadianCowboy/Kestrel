#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::runtime {

// Why recognition stopped producing text. A caller has to be able to tell
// "I heard nothing" from "there is no microphone" from "the engine failed",
// because those three need three different messages to the user.
enum class RecognitionEnd {
    Silence,     // the speaker stopped and the phrase was final
    Cancelled,   // the user or the app stopped listening
    NoAudio,     // the capture device produced nothing
    Failed,      // the engine itself failed; `detail` says how
};

[[nodiscard]] const char* toString(RecognitionEnd reason) noexcept;

// One recognition result, as it happens.
//
// Partial results are the whole reason a recognizer is streamed rather than
// called once at the end: a user who can see their words appear knows the
// microphone works before they have finished the sentence, and can correct
// themselves while there is still time to correct.
struct RecognitionResult {
    // The text so far. Grows across partial results and is repeated in full in
    // the final one, so a consumer never has to accumulate it itself.
    std::string text;
    // Confidence in [0, 1], reported by the engine and not invented here.
    double confidence = 0.0;
    bool isFinal = false;
    RecognitionEnd end = RecognitionEnd::Silence;
    std::string detail;
};

// A microphone and a recognizer behind one contract.
//
// Deliberately shaped like ModelBackend: generate() has a callback contract and
// cancellation rules, and every implementation must be safe to cancel from
// another thread. Recognition has exactly the same hazards -- a long call that
// cannot be interrupted, a callback that arrives after the caller gave up -- and
// learning the rules twice would be the worst outcome.
//
// Threading contract, identical in spirit to ModelBackend's:
//   * start()/stop() may be called from the UI thread. start() returns
//     immediately; recognition happens on the backend's own thread.
//   * onResult and onEnd are invoked on that thread, or at least never while
//     holding a lock the caller could deadlock on. Callers marshal back to their
//     own thread themselves.
//   * stop() must be safe while recognition is running, and must make results
//     stop arriving shortly afterwards. A result delivered after stop() is not
//     an error, but a caller must be able to ignore it.
//
// There is no platform adapter here yet. Everything above this contract is
// already complete and tested -- the listen session, the barge-in path, the
// interface -- and it drives MockSpeechRecognizer until a real recognizer is
// supplied. Adding one is the same shape as the CUDA device discovery: a
// translation unit that includes the platform header and nothing else does,
// reporting available() honestly so the app keeps working without it.
class SpeechRecognizer {
public:
    using ResultCallback = std::function<void(const RecognitionResult&)>;
    using EndCallback = std::function<void(RecognitionEnd, std::string_view)>;

    virtual ~SpeechRecognizer() = default;

    [[nodiscard]] virtual bool available() const = 0;
    // What the recognizer is and why it may be unavailable, for diagnostics.
    [[nodiscard]] virtual std::string detail() const = 0;

    // Begins listening. Refuses when unavailable, when already listening, or
    // when the callbacks are empty, and says which.
    virtual bool start(ResultCallback onResult, EndCallback onEnd, std::string& error) = 0;

    // Stops listening. Safe to call at any time, including from another thread
    // and including when not listening. Must not deliver a final result after
    // it returns unless the phrase was genuinely complete.
    virtual void stop() = 0;

    [[nodiscard]] virtual bool listening() const = 0;
};

// A recognizer that needs no microphone, for the preview build and for tests.
//
// It is not a stub in the sense of doing nothing: it walks a queue of scripted
// phrases and reports them the way a real recognizer does -- partial results
// first, one final result per phrase. That makes it useful for exercising the
// whole barge-in path on a build machine with no audio hardware, which is
// otherwise the part of the voice loop nobody can test.
class MockSpeechRecognizer final : public SpeechRecognizer {
public:
    // `phrases` are delivered in order. Each is reported as a series of partial
    // results split on word boundaries before the final one, so a consumer sees
    // the same shape of event it would from a live engine.
    explicit MockSpeechRecognizer(std::vector<std::string> phrases = {});

    [[nodiscard]] bool available() const override { return true; }
    [[nodiscard]] std::string detail() const override;
    bool start(ResultCallback onResult, EndCallback onEnd, std::string& error) override;
    void stop() override;
    [[nodiscard]] bool listening() const override { return m_listening; }

    // Emits the next partial result for the current phrase. Called by the
    // listen session on a timer, so the pacing of partials is the caller's
    // decision rather than a thread of its own.
    void emitNextPartial();

private:
    std::vector<std::string> m_phrases;
    std::size_t m_phrase = 0;
    std::size_t m_word = 0;
    bool m_listening = false;
    ResultCallback m_onResult;
    EndCallback m_onEnd;
};

} // namespace kestrel::runtime
