// The Windows half of the SAPI 5 dictation adapter.
//
// This is the only translation unit in the project that includes a SAPI header,
// for the same reason cudadiscovery_cuda.cpp is the only one that includes a
// CUDA header: the platform header stays behind the boundary, and everything
// above it is written against SpeechRecognizer.
//
// The whole pipeline, and every name in it is declared in the Windows SDK's
// sapi.h:
//
//   CoCreateInstance(CLSID_SpSharedRecognizer) -> ISpRecognizer
//   ISpRecognizer::SetInput(nullptr, SPADTYPE_INPUT)   the default microphone
//   ISpRecognizer::CreateRecoContext()       -> ISpRecoContext
//   ISpRecoContext::CreateGrammar(...)       -> ISpRecoGrammar
//   ISpRecoGrammar::SetDictationState(SPRS_ACTIVE)  hear anything at all
//   ISpEventSource::SetInterest(SPFEI(...) | ..., 0)
//   ISpEventSource::SetNotifyWin32Event()    -> an HANDLE to wait on
//   ISpEventSource::GetNotifyEventHandle()
//   ISpEventSource::GetEvents(...)           -> the events themselves
//   ISpRecoResult::GetText(...)              -> the words, via the interface
//
// Two things about that list are worth writing down, because both look like
// missing features and are not:
//
//   * There is no ISpeechRecognitionClient here. That interface, and
//     ISpRecoContext::SetAudioStreamSource, are SAPI 5.1 additions that live in
//     the Speech SDK rather than in the Windows SDK. The Windows SDK's sapi.h is
//     the 5.0 view of these interfaces, and the 5.0 view is the one the shared
//     engine implements: it takes its input device from SetInput, and its events
//     are pulled with GetEvents rather than pushed through a client object.
//
//   * Events are pulled rather than delivered to a callback. That is not a
//     stylistic choice. A callback would have to carry a pointer to its owner
//     through SAPI's wParam and would have to be released before it could be
//     called, which is the single easiest way to get a call into a
//     half-destroyed object on shutdown. A wait handle and a pull cannot.
//     Nothing here can run after the object that owns it is gone, because the
//     only thread that runs it is joined first.

#include "runtime/sapirecognizer.h"

#if defined(_WIN32)

#include <objbase.h>
#include <windows.h>

// sapi.h declares its CLSIDs and IIDs as externs unless INITGUID is defined
// first, in which case it defines them. CoCreateInstance below needs the
// definitions, so the order of these two includes is load bearing.
#include <initguid.h>
#include <mmsystem.h>
#include <sapi.h>
// For SpClearEvent, which is the documented counterpart to draining the event
// queue: it releases whatever the engine attached to an event. It is an inline
// function here rather than something to declare by hand, which is the point.
#include <sphelper.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace kestrel::runtime {
namespace {

// The events this recognizer acts on.
//
// SPFEI() is a macro from sapi.h that turns a SPEVENTENUM into the bit the
// interest mask wants: 1 << the enum. The mask is a set of *event indices*.
//
// This used to be hand-declared as SPRVI_SR and SPRVI_OTHER, which are
// SPRVIFLAGS values -- a different enum, describing things to do with a voice,
// not events to receive. The two namespaces overlap numerically and nothing
// complained: SetInterest succeeded, and the engine then raised almost nothing.
// A recognizer with the wrong interest mask is silent rather than broken, which
// is why it survived being written and why the comment here used to explain the
// wrong constants as if they were deliberate.
//
// SPFEI itself does come from sapi.h, so no spelling-out is needed.
constexpr ULONGLONG kInterest =
    SPFEI(SPEI_RECOGNITION)      // a phrase was recognised; carries the text
    | SPFEI(SPEI_SOUND_START)     // the engine heard something begin
    | SPFEI(SPEI_FALSE_RECOGNITION) // audio arrived and the engine rejected it
    | SPFEI(SPEI_END_SR_STREAM);  // the engine closed the phrase: normal end

// The audio category SetInput is told to look in, when the token is null and
// the engine picks its own default device. SPADTYPE_INPUT is a Speech SDK enum
// the Windows SDK does not ship, and it is one stable value.
constexpr DWORD kAudioCategoryInput = 1;

// How long one phrase may take before the session is ended as a failure.
//
// The engine ends a phrase itself when the speaker stops, so this is a backstop
// rather than the normal path. It exists because the alternative -- a recognizer
// that never ends a session -- leaves the interface sitting in a listening
// state forever, which is the one outcome worse than an error message.
constexpr std::chrono::seconds kPhraseDeadline{30};

// What SPEI_RECOGNITION actually carries, and the thing this file used to get
// wrong.
//
// There is a struct for this event in SAPI, called SPRECOCGNITION, and it is
// laid out here from the published definition: a closing SPEVENT, a confidence,
// a wide phrase pointer, an alternates pointer. It reads beautifully and it is
// not what arrives. SPRECOCGNITION belongs to the Speech SDK's sapi.h, not the
// Windows SDK's, and the Windows SDK delivers the event differently: lParam is
// an ISpRecoResult* COM object.
//
// Casting lParam to the struct is not a rough edge, it is memory corruption. The
// read takes the object's vtable pointer as the confidence and its first
// internal as the phrase pointer, and then CoTaskMemFree's that -- handing the
// COM task allocator a pointer it never allocated, on the hot path, once per
// recognised phrase. SpClearEvent afterwards releases an object that has
// already been damaged, so nothing reports the fault; the process just goes
// wrong somewhere later.
//
// The assertions that used to sit here pinned the offsets and made the file
// look checked. They pinned the offsets of a struct no event ever contains.
//
// So the text is read through the interface instead, which is the only way it
// can be read correctly. The cost is that confidence goes with it: the Speech
// SDK's ISpRecoResult::GetConfidence, and the SPRECOCGNITIONINFO it returns,
// are absent from the Windows SDK entirely -- not declared differently,
// absent -- so there is no confidence to report and none is invented.
// UTF-16 from the engine to UTF-8 for the interface. Reported, not repaired:
// a conversion failure yields no text, and a session with no text ends as
// NoAudio rather than as a phrase nobody said.
std::string toUtf8(const wchar_t* text) {
    if (text == nullptr || *text == L'\0') {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                           nullptr, 0, nullptr, nullptr);
    const int size = needed > 0
                         ? needed
                         : WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string out(static_cast<std::size_t>(size - 1), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                            out.data(), size, nullptr, nullptr);
    if (written <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written - 1));
    return out;
}

std::string formatHr(HRESULT hr) {
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

class RecognizerWorker;

// True on the recognizer's own worker thread, for its whole lifetime. A
// thread_local rather than a member: a consumer may legitimately call stop()
// from inside a callback, and waiting there for the thread doing the waiting
// would be a deadlock. There is exactly one worker thread, so a flag is
// enough and nothing has to be read across threads to find out.
thread_local bool t_inWorker = false;

// What one listening session has learned so far. Kept apart from the worker so
// that the audio objects can be released before anything else is touched: the
// context holds the only reference to the engine, and the engine is what calls
// back.
struct SessionState {
    std::string phrase;
    // Left at zero for every phrase, deliberately. The Windows SDK's
    // ISpRecoResult has no confidence accessor to read and none is invented
    // here; the field stays because the result contract has one and a future
    // build against the Speech SDK is the place to fill it.
    double confidence = 0.0;
    bool heardSound = false;
    // The engine received audio and declined to recognise it, as opposed to
    // there being no audio. The difference decides which of the two NoAudio
    // messages the user is given, and both of them name a different thing to
    // go and check.
    bool rejectedAudio = false;
    // Set by the engine closing the phrase, or by the microphone stopping. Both
    // end the session, and both are the engine's decision rather than a timeout.
    bool finished = false;
    std::string failure;
};

// The callbacks one session delivers through. They are paired with the session
// generation under m_mutex before the worker enters runSession(), then held for
// the whole pass. A stopped session can therefore never deliver through a
// replacement session's callbacks, even when stop() is followed immediately
// by start().
struct SessionCallbacks {
    SpeechRecognizer::ResultCallback onResult;
    SpeechRecognizer::EndCallback onEnd;
};

} // namespace

// The recognizer's worker thread and the state the UI thread shares with it.
//
// Threading, in one place so it can be checked:
//
//   UI thread    start(), stop(), listening(), available(), detail()
//   worker       everything COM: creating, the audio pipeline, the event pull,
//                releasing, and every callback invocation
//
// start() and stop() only publish state under a short mutex and signal an
// event; neither waits for COM or for the engine to open a device. stopAndWait()
// is reserved for session teardown, while the recognizer destructor sets the
// shutdown flag and joins the worker permanently.
class SapiSpeechRecognizer::Impl {
public:
    Impl() {
        m_devices = static_cast<unsigned>(waveInGetNumDevs());
        m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        // The wake handle is part of availability, not a detail. A recognizer
        // that cannot be stopped is worse than one that never started, so if
        // the event could not be created this adapter reports itself unavailable
        // rather than offering a listening state it cannot end.
        m_available = m_devices > 0 && m_wake != nullptr;
    }

    ~Impl() {
        {
            std::lock_guard<std::mutex> apiLock(m_apiMutex);
            std::lock_guard<std::mutex> lock(m_mutex);
            m_shuttingDown.store(true, std::memory_order_release);
            m_listening.store(false, std::memory_order_release);
            m_generation.fetch_add(1, std::memory_order_acq_rel);
            m_onResult = {};
            m_onEnd = {};
        }
        if (m_wake != nullptr) {
            SetEvent(m_wake);
        }
        if (m_thread.joinable()) {
            m_thread.join();
        }
        if (m_wake != nullptr) {
            CloseHandle(m_wake);
        }
    }

    [[nodiscard]] bool available() const { return m_available; }

    [[nodiscard]] std::string detail() const {
        if (!m_available) {
            return "Windows SAPI 5 is present but this machine reports no capture "
                   "device, so it cannot listen";
        }
        return "Windows SAPI 5 recognizer, " + std::to_string(m_devices)
             + " capture device(s), finished phrases only";
    }

    bool start(ResultCallback onResult, EndCallback onEnd, std::string& error) {
        std::lock_guard<std::mutex> apiLock(m_apiMutex);
        if (!m_available) {
            error = detail();
            return false;
        }
        if (!onResult || !onEnd) {
            error = "start needs both a result and an end callback";
            return false;
        }
        {
            // Callbacks and generation are published together. The worker takes
            // both under this same lock, so a stop/start between its readiness
            // check and session entry cannot pair an old generation with new
            // callbacks.
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_shuttingDown.load(std::memory_order_acquire)) {
                error = "recognizer is shutting down";
                return false;
            }
            if (m_listening.load(std::memory_order_acquire)) {
                error = "already listening";
                return false;
            }
            m_onResult = std::move(onResult);
            m_onEnd = std::move(onEnd);
            m_generation.fetch_add(1, std::memory_order_acq_rel);
            m_listening.store(true, std::memory_order_release);
        }
        if (m_thread.joinable()) {
            SetEvent(m_wake);
            return true;
        }
        // The thread reads the generation itself when it starts, so the first
        // session needs no wake-up. A thread that cannot be created is a
        // failure to start, not a listening state nobody can leave.
        try {
            m_thread = std::thread(&Impl::workerMain, this);
        } catch (const std::system_error&) {
            m_listening.store(false, std::memory_order_release);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_onResult = {};
                m_onEnd = {};
            }
            m_sessionFinished.notify_all();
            error = "the recognizer thread could not be started";
            return false;
        }
        return true;
    }

    void requestStop() {
        // The generation bump is what actually ends a session: the worker's
        // wait loop compares against it, so a stop is noticed even if the
        // worker is between two events rather than sitting in the wait.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_listening.store(false, std::memory_order_release);
            m_generation.fetch_add(1, std::memory_order_acq_rel);
            // If the worker has not claimed this session yet, there is nothing
            // to wait for and its callbacks must not leak into a later start.
            if (!m_sessionRunning) {
                m_onResult = {};
                m_onEnd = {};
            }
        }
        if (m_thread.joinable() && m_wake != nullptr) {
            SetEvent(m_wake);
        }
        m_sessionFinished.notify_all();
    }

    void stop() {
        std::lock_guard<std::mutex> apiLock(m_apiMutex);
        requestStop();
    }

    void stopAndWait() {
        if (t_inWorker) {
            requestStop();
            return;
        }
        std::lock_guard<std::mutex> apiLock(m_apiMutex);
        requestStop();
        // Keep starts excluded through this wait: a new session may not begin
        // against the same borrowed callback target while its teardown is in
        // progress. The worker needs only m_mutex, never m_apiMutex, to finish.
        std::unique_lock<std::mutex> lock(m_mutex);
        m_sessionFinished.wait(lock, [this] {
            return !m_sessionRunning && !m_listening.load(std::memory_order_acquire);
        });
    }

    [[nodiscard]] bool listening() const { return m_listening.load(std::memory_order_acquire); }

private:
    void workerMain() {
        // Set for this thread's whole lifetime, and the only thing that lets
        // stop() tell "called from a callback" apart from "called from
        // elsewhere" without a mutex on the hot path.
        t_inWorker = true;
        // SAPI's objects are apartment threaded, so the thread that creates
        // them has to own an apartment for their whole life. That is why
        // everything COM happens here rather than in start(), and why the last
        // release below is followed by CoUninitialize on this thread.
        const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comInit)) {
            // Keep the worker alive to report the failure through each session's
            // callbacks. start() remains asynchronous and the consumer receives
            // the reason on the same callback path as other SAPI failures.
            m_comFailure = formatHr(comInit);
        }

        for (;;) {
            if (m_shuttingDown.load(std::memory_order_acquire)) {
                break;
            }
            if (!m_listening.load(std::memory_order_acquire)) {
                if (!waitForWork()) {
                    break;
                }
                continue;
            }

            unsigned generation = 0;
            SessionCallbacks callbacks;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                if (!m_listening.load(std::memory_order_acquire)) {
                    continue;
                }
                generation = m_generation.load(std::memory_order_acquire);
                callbacks.onResult = std::move(m_onResult);
                callbacks.onEnd = std::move(m_onEnd);
                m_onResult = {};
                m_onEnd = {};
                m_sessionRunning = true;
            }
            if (FAILED(comInit)) {
                m_listening.store(false, std::memory_order_release);
                deliverEnd(callbacks, RecognitionEnd::Failed,
                           "COM could not be initialised on the recognizer thread: "
                               + m_comFailure);
            } else {
                runSession(generation, callbacks);
            }
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_sessionRunning = false;
            }
            m_sessionFinished.notify_all();
        }
        if (SUCCEEDED(comInit)) {
            CoUninitialize();
        }
    }

    // One pass of the wait loop with no session running. Returns false when the
    // recognizer is being destroyed, which is the only way the worker leaves.
    //
    // The timeout is a shutdown bound rather than a wait: a start or a stop
    // always signals m_wake after setting its flag, so the wait would otherwise
    // be exact, and bounding it means a lost wake-up costs a few milliseconds
    // of latency instead of a destructor that never returns.
    bool waitForWork() {
        drainMessages();
        const DWORD result = MsgWaitForMultipleObjects(1, &m_wake, FALSE, 200, QS_ALLINPUT);
        if (result == WAIT_FAILED) {
            return false;
        }
        if (result == WAIT_OBJECT_0 + 1) {
            drainMessages();
        }
        return !m_shuttingDown.load(std::memory_order_acquire);
    }

    void runSession(unsigned generation, const SessionCallbacks& callbacks) {
        SessionState state;

        ISpRecognizer* recognizer = nullptr;
        ISpRecoContext* context = nullptr;
        ISpRecoGrammar* grammar = nullptr;
        HANDLE events = nullptr;

        // The order of these steps is not a style choice. A shared recognizer
        // cannot create a recognition context until it has an input, so
        // SetInput comes first and its failure is the one worth reporting: the
        // context that follows fails too, with a code that says nothing about
        // the microphone being the problem.
        //
        // Every step keeps the name of what it was doing, because a bare status
        // code is not something a user can act on. "SAPI could not open the
        // microphone: 0x8004503A" is.
        const char* step_name = "creating the recognizer";
        HRESULT hr = CoCreateInstance(CLSID_SpSharedRecognizer, nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&recognizer));
        if (SUCCEEDED(hr)) {
            // A null token means the default capture device, which is the one
            // the user chose in the system sound settings.
            step_name = "opening the microphone";
            hr = recognizer->SetInput(nullptr, kAudioCategoryInput);
        }
        if (SUCCEEDED(hr)) {
            step_name = "creating a recognition context";
            hr = recognizer->CreateRecoContext(&context);
        }
        if (SUCCEEDED(hr)) {
            // This is the step that was missing, and nothing failed without it.
            //
            // A recognition context does not listen on its own. It recognises
            // only what its grammars tell it to recognise, and this adapter
            // created no grammar at all -- so the engine was handed a
            // microphone, a context, an interest mask and a set of event
            // handlers, and then had nothing to recognise with. It never
            // raised SPEI_RECOGNITION, so the code that reads a phrase never
            // ran, and the whole path sat there looking correct.
            step_name = "creating a dictation grammar";
            hr = context->CreateGrammar(1, &grammar);
        }
        if (SUCCEEDED(hr)) {
            // SPRS_ACTIVE, on the plain C interface, rather than the
            // ISpGrammarBuilder helper's LoadGrammar(L"", SPLOAD_AS_DICTATION).
            // Both do the same thing; the helper is the only place
            // SPLOAD_AS_DICTATION appears, and it lives in the Speech Platform
            // SDK, which is a separate download from the Windows SDK this file
            // is allowed to rely on. Spelling the enum out by hand is how the
            // interest mask above came to be wrong: the constant is not the
            // problem, guessing it is. SPRS_ACTIVE is in sapi.h, and it is the
            // whole of what "recognise whatever I say" means.
            step_name = "turning dictation on";
            hr = grammar->SetDictationState(SPRS_ACTIVE);
        }
        if (SUCCEEDED(hr)) {
            step_name = "asking the engine for recognition events";
            hr = context->SetInterest(kInterest, 0);
        }
        if (SUCCEEDED(hr)) {
            // An event handle rather than a callback, so the worker can wait on
            // it alongside its own wake-up handle and no engine pointer ever
            // has to outlive the thread that owns it.
            step_name = "arming the speech event handle";
            hr = context->SetNotifyWin32Event();
        }
        if (SUCCEEDED(hr)) {
            events = context->GetNotifyEventHandle();
            if (events == nullptr) {
                hr = E_FAIL;
            }
        }

        if (FAILED(hr)) {
            releaseAudio(context, grammar, recognizer);
            clearListeningIfCurrent(generation);
            deliverEnd(callbacks, RecognitionEnd::Failed,
                       std::string("SAPI could not ") + step_name + ": " + formatHr(hr));
            return;
        }

        // The engine closes a phrase itself once the speaker stops, so the
        // normal exit is the engine saying so. The deadline is the backstop for
        // the case where it never does.
        const auto deadline = std::chrono::steady_clock::now() + kPhraseDeadline;
        bool cancelled = false;
        for (;;) {
            if (m_shuttingDown.load(std::memory_order_acquire)
                || m_generation.load(std::memory_order_acquire) != generation) {
                cancelled = true;
                break;
            }
            if (state.finished) {
                break;
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       deadline - std::chrono::steady_clock::now())
                                       .count();
            if (remaining <= 0) {
                // Only a failure if something was heard. The engine ends a
                // phrase when the speaker stops, so running out of time with
                // silence means there was nothing to end -- and reporting that
                // as a failure tells the user their microphone is broken when
                // the truth is that nobody spoke, or that the input is muted.
                // Silence has its own outcome below, with its own wording.
                if (state.heardSound) {
                    state.failure = "the recognizer heard speech but did not finish "
                                    "a phrase within "
                                    + std::to_string(kPhraseDeadline.count())
                                    + " seconds";
                }
                break;
            }

            const HANDLE wait[2] = {events, m_wake};
            const DWORD result = MsgWaitForMultipleObjects(2, wait, FALSE,
                                                           static_cast<DWORD>(remaining),
                                                           QS_ALLINPUT);
            if (result == WAIT_OBJECT_0) {
                pullEvents(context, state);
            } else if (result == WAIT_OBJECT_0 + 1) {
                if (m_shuttingDown.load(std::memory_order_acquire)
                    || m_generation.load(std::memory_order_acquire) != generation) {
                    cancelled = true;
                    break;
                }
            } else if (result == WAIT_OBJECT_0 + 2) {
                drainMessages();
            } else if (result == WAIT_FAILED) {
                state.failure = "the wait on the speech engine failed";
                break;
            }
            // WAIT_TIMEOUT is the deadline being observed at the top of the
            // loop, where it is turned into a reason.
        }

        releaseAudio(context, grammar, recognizer);
        clearListeningIfCurrent(generation);

        if (cancelled) {
            // A stop is a cancellation, not a completed phrase. Reporting the
            // half-heard text here would hand the user a sentence they did not
            // finish as if they had.
            RecognitionResult stopped;
            stopped.isFinal = false;
            stopped.end = RecognitionEnd::Cancelled;
            deliverResult(callbacks, stopped);
            deliverEnd(callbacks, RecognitionEnd::Cancelled, {});
            return;
        }
        if (!state.failure.empty()) {
            deliverEnd(callbacks, RecognitionEnd::Failed, state.failure);
            return;
        }
        if (state.phrase.empty()) {
            deliverEnd(callbacks, RecognitionEnd::NoAudio,
                       state.rejectedAudio
                           ? "the speech engine heard audio and could not recognise it"
                           : (state.heardSound
                                  ? "the microphone heard no recognisable speech"
                                  : "the microphone heard nothing at all"));
            return;
        }

        RecognitionResult final;
        final.text = state.phrase;
        final.confidence = state.confidence;
        final.isFinal = true;
        final.end = RecognitionEnd::Silence;
        deliverResult(callbacks, final);
        deliverEnd(callbacks, RecognitionEnd::Silence, {});
    }

    // Clears the listening flag only while this session is still the current
    // one.
    //
    // The flag is a plain store in the obvious version of this, and the obvious
    // version is wrong for the same reason the callbacks are snapshotted: a
    // session that has already been superseded is still allowed to reach here,
    // and clearing the flag would tell the worker nobody is listening when the
    // session that replaced it very much is. The worker would then sit in its
    // wait with m_listening false and the new session would never start.
    void clearListeningIfCurrent(unsigned generation) {
        if (m_generation.load(std::memory_order_acquire) == generation) {
            m_listening.store(false, std::memory_order_release);
        }
    }

    // Reads whatever the engine has queued. Called on the worker thread with
    // the context it belongs to, so every pointer in here is valid for the
    // duration of the call.
    static void pullEvents(ISpRecoContext* context, SessionState& state) {
        ULONG available = 0;
        if (FAILED(context->GetEvents(0, nullptr, &available)) || available == 0) {
            return;
        }
        std::vector<SPEVENT> events(available);
        ULONG fetched = 0;
        if (FAILED(context->GetEvents(available, events.data(), &fetched))) {
            return;
        }
        for (ULONG i = 0; i < fetched; ++i) {
            const SPEVENT& event = events[i];
            switch (event.eEventId) {
            case SPEI_SOUND_START:
                state.heardSound = true;
                break;
            case SPEI_FALSE_RECOGNITION:
                // The engine got audio and could not make words out of it. That
                // is the one case where "the microphone heard nothing at all"
                // is exactly the wrong thing to tell the user: the audio
                // arrived, and what failed was recognition. Counting it as
                // heard is what makes the end reason say so.
                state.heardSound = true;
                state.rejectedAudio = true;
                break;
            case SPEI_RECOGNITION: {
                // lParam is a LONG_PTR, so the pointer has to be cast through
                // an integer type. const_cast cannot do it: the value is not
                // const, it is a pointer stored in a field that happens to be
                // named like one.
                auto* result = reinterpret_cast<ISpRecoResult*>(
                    static_cast<std::uintptr_t>(event.lParam));
                if (result == nullptr) {
                    break;
                }
                // GetText rather than a field read, because there is no field.
                // Zero and zero mean the whole phrase -- ulStart is a word index
                // and ulCount of zero means "every word from there on" -- and
                // TRUE asks for the text replacements, so a phrase with a
                // corrected homophone reads as the corrected word.
                wchar_t* raw = nullptr;
                std::string text;
                if (SUCCEEDED(result->GetText(0, 0, TRUE, &raw, nullptr))
                    && raw != nullptr) {
                    text = toUtf8(raw);
                    // The allocation is the COM task allocator's, and
                    // CoTaskMemFree is the only thing that can release it:
                    // freeing it with the C runtime would corrupt a heap the
                    // engine still owns. One of these arrives per recognised
                    // phrase, so this is the hot path rather than an edge.
                    ::CoTaskMemFree(raw);
                }
                if (text.empty()) {
                    break;
                }
                // Recognition supplies a completed phrase; the input stream
                // may remain open, so do not wait for it to end.
                state.phrase = text;
                state.finished = true;
                // No confidence is set, and the reason is in the note above the
                // old SPRECOCGNITION declaration: the Windows SDK's
                // ISpRecoResult has no accessor for one. The field stays at its
                // default rather than being filled with a plausible number.
                break;
            }
            case SPEI_END_SR_STREAM: {
                // The engine closed the phrase: the speaker stopped and it
                // decided that was the end. This is the normal exit.
                state.finished = true;
                // lParam carries the HRESULT the stream ended with. A capture
                // device that was unplugged, or a format the engine could not
                // keep up with, ends the stream in failure, and reporting that
                // as a completed phrase would tell the user their words were
                // heard when the engine never received them. The session still
                // finishes -- the audio is gone either way -- but it finishes
                // with the reason attached.
                const HRESULT streamEnd = static_cast<HRESULT>(event.lParam);
                if (FAILED(streamEnd) && state.failure.empty()) {
                    state.failure =
                        "the speech engine ended the input stream in failure: "
                        + formatHr(streamEnd);
                }
                break;
            }
            default:
                break;
            }
            // Releases whatever the engine attached to this event, and only for
            // events it actually populated. SpClearEvent is the documented
            // counterpart to a queue read and is safe on an event whose
            // eEventId carries no data: it checks lObject and wParam itself.
            // Without it every event the engine queues costs a leak, and a
            // session's worth of them is a session's worth of engine memory
            // that never comes back.
            ::SpClearEvent(&events[i]);
        }
    }

    // Dependency order, not creation order. The context is created from the
    // recognizer and holds the reference to the audio device, so it is released
    // first; releasing the recognizer underneath a live context is the mistake
    // that leaves a dangling engine reference behind. Interest is turned off
    // before any of it, so nothing is delivered into an object that is on its
    // way out.
    static void releaseAudio(ISpRecoContext* context, ISpRecoGrammar* grammar,
                             ISpRecognizer* recognizer) {
        if (context != nullptr) {
            context->SetInterest(0, 0);
        }
        // The grammar before the context that created it, the context before
        // the recognizer that owns the audio device. Releasing them the other
        // way round leaves the context holding a grammar that is already gone.
        if (grammar != nullptr) {
            grammar->SetDictationState(SPRS_INACTIVE);
            grammar->Release();
        }
        if (context != nullptr) {
            context->Release();
        }
        if (recognizer != nullptr) {
            recognizer->Release();
        }
    }

    static void drainMessages() {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }

    // Invoked on the worker thread with this session's own callbacks, holding
    // no lock, so a consumer is free to call back into the recognizer -- and
    // free to stop it, which is the common way a listening session ends.
    void deliverResult(const SessionCallbacks& callbacks,
                       const RecognitionResult& result) {
        if (callbacks.onResult) {
            callbacks.onResult(result);
        }
    }

    void deliverEnd(const SessionCallbacks& callbacks, RecognitionEnd reason,
                    const std::string& detail) {
        if (callbacks.onEnd) {
            callbacks.onEnd(reason, detail);
        }
    }

    unsigned m_devices = 0;
    bool m_available = false;
    HANDLE m_wake = nullptr;
    std::thread m_thread;
    // Bumped by both start() and stop(). A session runs against the value it
    // started with and ends the moment it no longer matches, which is what makes
    // a stop take effect between two engine events as well as during the wait.
    std::atomic<unsigned> m_generation{0};
    std::atomic<bool> m_listening{false};
    std::atomic<bool> m_shuttingDown{false};
    std::mutex m_apiMutex;
    std::mutex m_mutex;
    std::condition_variable m_sessionFinished;
    bool m_sessionRunning = false;
    std::string m_comFailure;
    ResultCallback m_onResult;
    EndCallback m_onEnd;
};

SapiSpeechRecognizer::SapiSpeechRecognizer()
    : m_impl(std::make_unique<Impl>()) {}

SapiSpeechRecognizer::~SapiSpeechRecognizer() = default;

bool SapiSpeechRecognizer::available() const {
    return m_impl->available();
}

std::string SapiSpeechRecognizer::detail() const {
    return m_impl->detail();
}

bool SapiSpeechRecognizer::start(ResultCallback onResult, EndCallback onEnd, std::string& error) {
    return m_impl->start(std::move(onResult), std::move(onEnd), error);
}

void SapiSpeechRecognizer::stop() {
    m_impl->stop();
}

void SapiSpeechRecognizer::stopAndWait() {
    m_impl->stopAndWait();
}

bool SapiSpeechRecognizer::listening() const {
    return m_impl->listening();
}

unsigned microphoneDeviceCount() {
    return static_cast<unsigned>(waveInGetNumDevs());
}

Microphone probeMicrophone() {
    return microphoneDeviceCount() > 0 ? Microphone::Present : Microphone::Absent;
}

std::unique_ptr<SpeechRecognizer> makePlatformSpeechRecognizer() {
    return std::make_unique<SapiSpeechRecognizer>();
}

} // namespace kestrel::runtime

#endif // _WIN32
