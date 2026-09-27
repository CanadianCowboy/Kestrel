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
//   ISpEventSource::SetInterest(SPRVI_SR | SPRVI_OTHER, 0)
//   ISpEventSource::SetNotifyWin32Event()    -> an HANDLE to wait on
//   ISpEventSource::GetNotifyEventHandle()
//   ISpEventSource::GetEvents(...)           -> the events themselves
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

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

namespace kestrel::runtime {
namespace {

// SPRVI_* is the SPRVIFLAGS enum, which is a Speech SDK header the Windows SDK
// does not ship. These are its published values, and they are load bearing: the
// interest mask decides which events the engine raises at all, so a wrong bit
// produces a recognizer that hears nothing and says nothing about why.
constexpr ULONGLONG kInterestRecognition = 0x0001; // SPRVI_SR
constexpr ULONGLONG kInterestOther = 0x0010;      // SPRVI_OTHER

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

// The payload of SPEI_RECOGNITION. SPRECOCGNITION belongs to the Speech SDK's
// sapi.h, so its layout is declared here from the published SAPI 5 definition.
// The two asserts pin the offsets that matter: a mistake in this file then
// fails the build rather than producing a misread pointer at run time.
struct RecognitionPayload {
    SPEVENT streamEnd;        // the SPEI_END_SR_STREAM that closes this phrase
    LRESULT confidence;       // 0..1000, the scale SAPI reports on
    wchar_t* phrase;          // the words, NUL terminated
    void* alternates;         // unused: one phrase per turn is enough
};
static_assert(offsetof(RecognitionPayload, confidence) == sizeof(SPEVENT),
              "SPEVENT is not the first member, or the SDK added padding");
static_assert(offsetof(RecognitionPayload, phrase) == sizeof(SPEVENT) + sizeof(LRESULT),
              "the phrase pointer does not follow the confidence");

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

// What one listening session has learned so far. Kept apart from the worker so
// that the audio objects can be released before anything else is touched: the
// context holds the only reference to the engine, and the engine is what calls
// back.
struct SessionState {
    std::string phrase;
    double confidence = 0.0;
    bool heardSound = false;
    // Set by the engine closing the phrase, or by the microphone stopping. Both
    // end the session, and both are the engine's decision rather than a timeout.
    bool finished = false;
    std::string failure;
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
// start() and stop() do nothing but flip an atomic and set an event, so neither
// can block the UI on a COM call or on the engine taking its time opening a
// device. The destructor sets the shutdown flag, signals the same event, and
// joins; that is the only place the UI waits, and by then it is shutting down.
class SapiSpeechRecognizer::Impl {
public:
    Impl() {
        m_devices = static_cast<unsigned>(waveInGetNumDevs());
        m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        // The wake handle is part of availability, not a detail. A recognizer
        // that cannot be stopped is worse than one that never started, so if the
        // event could not be created this adapter reports itself unavailable
        // rather than offering a listening state it cannot end.
        m_available = m_devices > 0 && m_wake != nullptr;
    }

    ~Impl() {
        m_shuttingDown.store(true, std::memory_order_release);
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
        if (!m_available) {
            error = detail();
            return false;
        }
        if (m_listening.load(std::memory_order_acquire)) {
            error = "already listening";
            return false;
        }
        if (!onResult || !onEnd) {
            error = "start needs both a result and an end callback";
            return false;
        }
        {
            // The worker takes a copy before it invokes anything and clears the
            // member first, so a consumer that stops the recognizer from inside
            // its own callback cannot leave a second call through an empty
            // std::function.
            std::lock_guard<std::mutex> lock(m_mutex);
            m_onResult = std::move(onResult);
            m_onEnd = std::move(onEnd);
        }
        m_listening.store(true, std::memory_order_release);
        m_generation.fetch_add(1, std::memory_order_acq_rel);
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
            error = "the recognizer thread could not be started";
            return false;
        }
        return true;
    }

    void stop() {
        // The generation bump is what actually ends a session: the worker's
        // wait loop compares against it, so a stop is noticed even if the
        // worker is between two events rather than sitting in the wait.
        m_listening.store(false, std::memory_order_release);
        m_generation.fetch_add(1, std::memory_order_acq_rel);
        if (m_thread.joinable() && m_wake != nullptr) {
            SetEvent(m_wake);
        }
    }

    [[nodiscard]] bool listening() const { return m_listening.load(std::memory_order_acquire); }

private:
    void workerMain() {
        // SAPI's objects are apartment threaded, so the thread that creates
        // them has to own an apartment for their whole life. That is why
        // everything COM happens here rather than in start(), and why the last
        // release below is followed by CoUninitialize on this thread.
        const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comInit)) {
            m_listening.store(false, std::memory_order_release);
            deliverEnd(RecognitionEnd::Failed,
                       "COM could not be initialised on the recognizer thread: "
                           + formatHr(comInit));
            return;
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
            runSession(m_generation.load(std::memory_order_acquire));
        }
        CoUninitialize();
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

    void runSession(unsigned generation) {
        SessionState state;

        ISpRecognizer* recognizer = nullptr;
        ISpRecoContext* context = nullptr;
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
            step_name = "asking the engine for recognition events";
            hr = context->SetInterest(kInterestRecognition | kInterestOther, 0);
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
            releaseAudio(context, recognizer);
            m_listening.store(false, std::memory_order_release);
            deliverEnd(RecognitionEnd::Failed,
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
                state.failure = "the recognizer did not finish a phrase within "
                                + std::to_string(kPhraseDeadline.count()) + " seconds";
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

        releaseAudio(context, recognizer);
        m_listening.store(false, std::memory_order_release);

        if (cancelled) {
            // A stop is a cancellation, not a completed phrase. Reporting the
            // half-heard text here would hand the user a sentence they did not
            // finish as if they had.
            RecognitionResult stopped;
            stopped.isFinal = false;
            stopped.end = RecognitionEnd::Cancelled;
            deliverResult(stopped);
            deliverEnd(RecognitionEnd::Cancelled, {});
            return;
        }
        if (!state.failure.empty()) {
            deliverEnd(RecognitionEnd::Failed, state.failure);
            return;
        }
        if (state.phrase.empty()) {
            deliverEnd(RecognitionEnd::NoAudio,
                       state.heardSound ? "the microphone heard no recognisable speech"
                                        : "the microphone heard nothing at all");
            return;
        }

        RecognitionResult final;
        final.text = state.phrase;
        final.confidence = state.confidence;
        final.isFinal = true;
        final.end = RecognitionEnd::Silence;
        deliverResult(final);
        deliverEnd(RecognitionEnd::Silence, {});
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
            case SPEI_RECOGNITION: {
                const auto* payload =
                    reinterpret_cast<const RecognitionPayload*>(event.lParam);
                if (payload == nullptr) {
                    break;
                }
                const std::string text = toUtf8(payload->phrase);
                if (text.empty()) {
                    break;
                }
                // The engine revises a phrase while the speaker is still
                // talking, so the latest one wins and only the last is ever
                // delivered. This adapter reports a finished phrase, not the
                // words so far; ListenSession already treats partial text as
                // optional.
                state.phrase = text;
                // SAPI reports confidence on a 0..1000 scale and the interface
                // promises [0, 1], so it is converted rather than reported raw.
                state.confidence = static_cast<double>(payload->confidence) / 1000.0;
                break;
            }
            case SPEI_END_SR_STREAM:
                // The engine closed the phrase: the speaker stopped and it
                // decided that was the end. This is the normal exit.
                state.finished = true;
                break;
            default:
                break;
            }
        }
    }

    // Dependency order, not creation order. The context is created from the
    // recognizer and holds the reference to the audio device, so it is released
    // first; releasing the recognizer underneath a live context is the mistake
    // that leaves a dangling engine reference behind. Interest is turned off
    // before any of it, so nothing is delivered into an object that is on its
    // way out.
    static void releaseAudio(ISpRecoContext* context, ISpRecognizer* recognizer) {
        if (context != nullptr) {
            context->SetInterest(0, 0);
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

    // Copies the callback out and clears the member before invoking it, so a
    // callback that stops the recognizer cannot leave a second invocation
    // pointing at an empty std::function.
    void deliverResult(const RecognitionResult& result) {
        ResultCallback callback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            callback = m_onResult;
            m_onResult = {};
        }
        if (callback) {
            callback(result);
        }
    }

    void deliverEnd(RecognitionEnd reason, const std::string& detail) {
        EndCallback callback;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            callback = m_onEnd;
            m_onEnd = {};
        }
        if (callback) {
            callback(reason, detail);
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
    std::mutex m_mutex;
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
