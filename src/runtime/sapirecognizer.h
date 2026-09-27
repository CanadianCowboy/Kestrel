#pragma once

#include "runtime/speechrecognizer.h"

#include <memory>
#include <string>

namespace kestrel::runtime {

// Dictation through SAPI 5 -- the desktop speech engine that has shipped with
// Windows since 2000, and the engine System.Speech is a thin wrapper over
// rather than an engine in its own right.
//
// Why SAPI and not Windows.Media.SpeechRecognition
// -------------------------------------------------
// The Windows.Media.SpeechRecognition projection in the Windows SDK (checked
// against 10.0.26100.0) exposes the ISpeechRecognizer ABI interface and no
// projected runtime class: there is no SpeechRecognizer::GetSpeechRecognizerAsync
// to call, and no activation factory, so RoCreateInstance on it has nothing to
// activate. The desktop flavour of that API has no interim-text event either.
// SAPI has been looked at and is the path that works: CLSID_SpSharedRecognizer
// and CLSID_SpFileStream are ordinary COM classes with real CoCreateInstance
// activation, they are in the Windows SDK, and the low-level interface the
// Speech SDK sits on top of -- ISpRecognizer and the ISpRecoContext it creates
// -- is declared in sapi.h rather than in a package that has to be installed.
// That also means no sapi.lib: every call below is a vtable call through an
// interface, so the only library the adapter needs is winmm, for the device
// count in the probe.
//
// What it does and does not give you
// ----------------------------------
// A finished phrase, a real confidence, and a real end reason, which is what
// SpeechRecognizer needs to end a session. It does not give interim text, so
// this adapter reports one final result per phrase. That is a smaller promise
// than the interface's own comment about partials makes, and it is stated here
// rather than faked: ListenSession already treats partial text as optional, and
// a user who waits a second longer for a word they already said is an
// inconvenience, not a broken microphone.
//
// The rules an adapter like this has to follow, which the next one should not
// have to rediscover:
//
//   * COM activation blocks. It runs on a thread of the recognizer's own,
//     never on the UI thread, which is why available() is answered from a
//     device count instead of by trying it.
//   * SAPI delivers its events on the thread that created the event source.
//     That thread must run a message pump, so the worker waits with
//     MsgWaitForMultipleObjects rather than sitting in a bare loop.
//   * Nothing is released from the UI thread. The recognizer, the context and
//     the event handle are all created and released on the worker, in the
//     reverse of the order they were created in, and CoUninitialize happens
//     after the last release. The destructor joins that thread, so no COM
//     object outlives the apartment it was created in.
//   * The microphone is chosen before the context is created, not after. A
//     shared recognizer has no context to create until it has an input, so a
//     SetInput failure is the one that has to be reported: the context that
//     follows it fails too, with a code that says nothing about the
//     microphone being the problem.
//   * A callback is released before it is invoked. A consumer that calls
//     stop() from inside its own callback is ordinary -- it is what happens
//     when a recognized phrase starts a turn and the session tears the
//     microphone down -- and the worker must not then call through a
//     std::function that has already been emptied.
class SapiSpeechRecognizer final : public SpeechRecognizer {
public:
    SapiSpeechRecognizer();
    ~SapiSpeechRecognizer() override;

    SapiSpeechRecognizer(const SapiSpeechRecognizer&) = delete;
    SapiSpeechRecognizer& operator=(const SapiSpeechRecognizer&) = delete;

    [[nodiscard]] bool available() const override;
    [[nodiscard]] std::string detail() const override;

    // Returns as soon as the worker has been woken. Everything that can fail --
    // creating the recognizer, opening the microphone -- fails on the worker,
    // and a failure arrives as an end event carrying the HRESULT, not as a
    // synchronous error here.
    bool start(ResultCallback onResult, EndCallback onEnd, std::string& error) override;
    void stop() override;
    [[nodiscard]] bool listening() const override;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// Whether this machine has a capture device. A device count, deliberately: it is
// a fast kernel call that answers the question the selection asks without
// opening anything, so probing cannot block the caller or leave a half-open
// device behind. It can be wrong in one direction -- a machine can advertise a
// capture endpoint that then refuses to open -- and the adapter reports that
// honestly as a start failure rather than pretending to be listening.
//
// It can also be wrong in the other direction, on a machine whose audio stack
// exposes capture endpoints only through WASAPI and not through the legacy wave
// interface this counts. KESTREL_SPEECH_INPUT=platform is the way past that: it
// asks for the adapter by name, and the adapter then reports what the engine
// says instead of what a device count guessed.
[[nodiscard]] Microphone probeMicrophone();

[[nodiscard]] unsigned microphoneDeviceCount();

// The platform recognizer, or a stand-in that reports unavailable when this
// build cannot talk to SAPI. Never returns null: a caller that wants the real
// one asks available() and detail(), which is how a missing optional
// dependency degrades what the app can do rather than whether it starts.
[[nodiscard]] std::unique_ptr<SpeechRecognizer> makePlatformSpeechRecognizer();

} // namespace kestrel::runtime
