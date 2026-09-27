#pragma once

#include "runtime/speechrecognizer.h"

#include <atomic>
#include <memory>
#include <string>

namespace kestrel::runtime {

// Speech recognition through Windows.Media.SpeechRecognition, the API Windows
// itself uses for dictation.
//
// This is the platform adapter for SpeechRecognizer, and it is the only
// translation unit in the project that includes a Windows Runtime header. The
// rule that keeps the rest of the code portable is the same one CUDA device
// discovery and the TensorRT backend follow: the platform header stays here,
// and everything above it is written against the interface.
//
// Why the shipped implementation reports that it cannot listen
// --------------------------------------------------------------
// The C++/Winrt projection that ships with the Windows SDK (checked against
// 10.0.26100.0) exposes the ISpeechRecognizer interface but no projected
// runtime class. There is no SpeechRecognizer::GetSpeechRecognizerAsync, no
// InterimResultsEnabled, and no ResultReceived event to call, so the adapter
// cannot be written against the projected API at all; reaching the engine means
// creating the ABI interface by hand with RoCreateInstance and calling its
// vtable by hand after that.
//
// The SDK's projection is also the desktop flavour of the API, whose status
// enum is SpeechRecognizerState (Idle, Capturing, SoundStarted, SoundEnded,
// SpeechDetected, Paused) rather than the UWP SpeechRecognizerStatus. That
// flavour reports when speech started and stopped, but it has no interim-text
// event at all: a result arrives only once the phrase is finished.
//
// So a real adapter built this way would satisfy the contract only in part. It
// could deliver a final result, a real confidence from RawConfidence, and the
// correct end reason; it could not deliver the partial results the interface
// documents, and those are what let a user see that the microphone works and
// correct themselves mid-sentence.
//
// Rather than ship something that compiles and quietly does less than the
// interface promises, this reports unavailable and the app keeps driving
// MockSpeechRecognizer, whose partial results are real. The seam above it --
// the interface, the listen session, the barge-in path -- is complete and tested
// and does not change when a real engine arrives.
//
// What a real adapter has to get right, so the next one does not rediscover it:
//
//   * SpeechRecognizer is not agile. It delivers its events through the
//     DispatcherQueue of the thread that created it, so the recognizer and a
//     message pump must live together on a thread of their own. The UI thread
//     cannot be that thread, because it is busy drawing.
//   * Blocking on RecognizeAsync() from the pump thread deadlocks the delivery
//     of the very events being waited for. Start the operation, return to the
//     pump, and let the operation's completion handler end the session.
//   * Recognition has no cancellation beyond Close(). A user who changes their
//     mind gets a stopped session, not a killed one.
//   * Every callback must be released before it is invoked. A consumer that
//     calls stop() from inside its own callback is ordinary, and calling back
//     into a half-destroyed session is not.
class WinRtSpeechRecognizer final : public SpeechRecognizer {
public:
    WinRtSpeechRecognizer();
    ~WinRtSpeechRecognizer() override;

    WinRtSpeechRecognizer(const WinRtSpeechRecognizer&) = delete;
    WinRtSpeechRecognizer& operator=(const WinRtSpeechRecognizer&) = delete;

    [[nodiscard]] bool available() const override;
    [[nodiscard]] std::string detail() const override;

    bool start(ResultCallback onResult, EndCallback onEnd, std::string& error) override;
    void stop() override;
    [[nodiscard]] bool listening() const override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// Constructs a recognizer backed by the platform, or nullptr when this build
// cannot talk to it. The caller is expected to say so rather than to fall back
// silently, which is why this returns null instead of substituting the mock.
[[nodiscard]] std::unique_ptr<SpeechRecognizer> makePlatformSpeechRecognizer();

} // namespace kestrel::runtime
