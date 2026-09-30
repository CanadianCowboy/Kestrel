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

// Whether this machine has something to listen with.
//
// A microphone is a fact about the machine, not about the code, so it is passed
// into the decision rather than probed inside it. That is what makes the choice
// between a real recognizer and the mock provable on a build machine with no
// audio hardware at all.
enum class Microphone {
    Present,
    Absent,
};

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
    // Confidence in [0, 1], as the engine reported it. Zero where the engine
    // reports none, which is a real answer rather than a rounded-up one: the
    // Windows SDK's SAPI 5 view has no accessor to ask.
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
//   * stop() is nonblocking and may be followed by a fresh start. Results from
//     the stopped session must not be confused with the new session's results.
//     Any callback already in flight is still safe for the consumer to ignore.
//   * stopAndWait() is teardown-only and must return after all callbacks from
//     the previous session have left the recognizer.
//
// The platform adapter is SapiSpeechRecognizer, declared in
// runtime/sapirecognizer.h. It reports available() honestly, so the app keeps
// working on a machine or a build where it cannot listen, and everything above
// this contract -- the listen session, the barge-in path, the interface -- is
// identical whichever recognizer is underneath it.
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

    // Lifetime boundary only: no callback may remain in flight on return.
    // UI cancellation uses stop(); consumers call this before destroying the
    // callback target. Implementations must provide a real join/wait boundary.
    virtual void stopAndWait() = 0;

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
    void stopAndWait() override { stop(); }
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

// The decision that picks a recognizer, with no platform code in it.
//
// Split out from makeBestSpeechRecognizer() so it can be asserted directly: the
// one thing worth testing here is that a machine with a microphone gets the
// platform adapter and a machine without one gets the mock, and neither
// conclusion may depend on the audio hardware of whatever machine runs the
// test.
[[nodiscard]] bool preferPlatformRecognizer(Microphone microphone) noexcept;

// Builds the recognizer a given microphone calls for.
//
// Returns the platform adapter when one is both preferred and usable, and the
// mock otherwise. There is no third outcome: a caller always gets something it
// can start(), and what it got says so through detail().
[[nodiscard]] std::unique_ptr<SpeechRecognizer> makeRecognizerFor(Microphone microphone);

// The recognizer this machine should actually use: the microphone probe and the
// decision, or an explicit branch when the caller knows better.
//
// Reading the environment is the caller's job, not this layer's. An app already
// has a way to read it -- KESTREL_SPEECH_INPUT, passed down as this -- and a
// portable layer that reached for the process environment itself would be one
// more thing to stub in a test.
enum class SpeechInput {
    Auto,     // probe the machine and decide
    Mock,     // always the scripted preview recognizer
    Platform, // always the platform adapter, which reports honestly if it
              // cannot listen rather than quietly substituting the mock
};

[[nodiscard]] std::unique_ptr<SpeechRecognizer> makeBestSpeechRecognizer(SpeechInput preference = SpeechInput::Auto);

} // namespace kestrel::runtime
