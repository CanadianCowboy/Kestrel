// The Windows half of the SAPI 5 dictation adapter.
//
// This is the only translation unit in the project that includes a SAPI header,
// for the same reason cudadiscovery_cuda.cpp is the only one that includes a
// CUDA header: the platform header stays behind the boundary, and everything
// above it is written against SpeechRecognizer.
//
// The shared recognizer uses the user's configured audio input. A context with
// an active dictation grammar supplies recognition events through a wait handle.
// All COM objects and event payloads are owned and released by the worker.

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

#include <algorithm>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

namespace kestrel::runtime {
namespace {

// How long one phrase may take before the session is ended as a failure.
//
// The engine ends a phrase itself when the speaker stops, so this is a backstop
// rather than the normal path. It exists because the alternative -- a recognizer
// that never ends a session -- leaves the interface sitting in a listening
// state forever, which is the one outcome worse than an error message.
constexpr std::chrono::seconds kPhraseDeadline{30};

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
    std::string out(static_cast<std::size_t>(size), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, -1,
                                            out.data(), size, nullptr, nullptr);
    if (written <= 0) {
        return {};
    }
    out.resize(static_cast<std::size_t>(written - 1));
    return out;
}

// Release SAPI event payloads according to their ownership tag. Kept here so
// builds need only sapi.h, including SDKs without the SpClearEvent helper.
void clearEvent(SPEVENT& event) {
    if (event.lParam != 0) {
        switch (event.elParamType) {
        case SPET_LPARAM_IS_TOKEN:
        case SPET_LPARAM_IS_OBJECT:
            reinterpret_cast<IUnknown*>(event.lParam)->Release();
            break;
        case SPET_LPARAM_IS_POINTER:
        case SPET_LPARAM_IS_STRING:
            CoTaskMemFree(reinterpret_cast<void*>(event.lParam));
            break;
        default:
            break;
        }
    }
    event = {};
}

/// Formats an HRESULT as a hexadecimal diagnostic code.
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
    /// Counts capture devices and creates the event used to wake the recognition worker.
    Impl() {
        m_devices = static_cast<unsigned>(waveInGetNumDevs());
        m_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        // The wake handle is part of availability, not a detail. A recognizer
        // that cannot be stopped is worse than one that never started, so if the
        // event could not be created this adapter reports itself unavailable
        // rather than offering a listening state it cannot end.
        m_available = m_devices > 0 && m_wake != nullptr;
    }

    /// Signals shutdown, joins the recognition worker, and closes its wake handle.
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

    /// Returns whether capture devices and a worker wake handle were found during setup.
    [[nodiscard]] bool available() const { return m_available; }

    /// Describes capture-device availability and the adapter's final-phrase-only output.
    [[nodiscard]] std::string detail() const {
        if (!m_available) {
            return "Windows SAPI 5 is present but this machine reports no capture "
                   "device, so it cannot listen";
        }
        return "Windows SAPI 5 recognizer, " + std::to_string(m_devices)
             + " capture device(s), finished phrases only";
    }

    /// Stores callbacks and starts or wakes the worker; reports invalid state or startup failure.
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
            m_callbackGeneration = m_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
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
            error = "the recognizer thread could not be started";
            return false;
        }
        return true;
    }

    /// Invalidates the active session and wakes the worker without waiting for COM cleanup.
    void stop() {
        // The generation bump is what actually ends a session: the worker's
        // wait loop compares against it, so a stop is noticed even if the
        // worker is between two events rather than sitting in the wait.
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_generation.fetch_add(1, std::memory_order_acq_rel);
            m_listening.store(false, std::memory_order_release);
        }
        if (m_thread.joinable() && m_wake != nullptr) {
            SetEvent(m_wake);
        }
    }

    /// Returns the atomic listening flag for the current recognition request.
    [[nodiscard]] bool listening() const { return m_listening.load(std::memory_order_acquire); }

private:
    /// Owns COM initialization and repeatedly runs requested sessions until shutdown.
    void workerMain() {
        // SAPI's objects are apartment threaded, so the thread that creates
        // them has to own an apartment for their whole life. That is why
        // everything COM happens here rather than in start(), and why the last
        // release below is followed by CoUninitialize on this thread.
        const HRESULT comInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(comInit)) {
            EndCallback onEnd;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                onEnd = std::move(m_onEnd);
                m_onResult = {};
                m_listening.store(false, std::memory_order_release);
            }
            if (onEnd) {
                onEnd(RecognitionEnd::Failed,
                      "COM could not be initialised on the recognizer thread: " + formatHr(comInit));
            }
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

    /// Runs one SAPI session on the worker and reports its phrase, cancellation, or failure.
    void runSession(unsigned generation) {
        ResultCallback onResult;
        EndCallback onEnd;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_callbackGeneration != generation) {
                return;
            }
            onResult = std::move(m_onResult);
            onEnd = std::move(m_onEnd);
        }
        // These callbacks belong to this start(), even if a new start arrives
        // while the old worker is releasing its COM objects.
        const auto finishListening = [this, generation] {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (m_generation.load(std::memory_order_acquire) == generation) {
                m_listening.store(false, std::memory_order_release);
            }
        };
        SessionState state;

        ISpRecognizer* recognizer = nullptr;
        ISpRecoContext* context = nullptr;
        ISpRecoGrammar* grammar = nullptr;
        HANDLE events = nullptr;

        // The shared recognizer owns its input; activate dictation on our context.
        const char* step_name = "creating the recognizer";
        HRESULT hr = CoCreateInstance(CLSID_SpSharedRecognizer, nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&recognizer));
        if (SUCCEEDED(hr)) {
            step_name = "creating a recognition context";
            hr = recognizer->CreateRecoContext(&context);
        }
        if (SUCCEEDED(hr)) {
            step_name = "asking the engine for recognition events";
            const ULONGLONG interests = SPFEI(SPEI_RECOGNITION)
                | SPFEI(SPEI_FALSE_RECOGNITION) | SPFEI(SPEI_SOUND_START);
            hr = context->SetInterest(interests, interests);
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

        if (SUCCEEDED(hr)) {
            step_name = "creating a dictation grammar";
            hr = context->CreateGrammar(0, &grammar);
        }
        if (SUCCEEDED(hr)) {
            step_name = "loading dictation";
            hr = grammar->LoadDictation(nullptr, SPLO_STATIC);
        }
        if (SUCCEEDED(hr)) {
            step_name = "activating dictation";
            hr = grammar->SetDictationState(SPRS_ACTIVE);
        }

        if (FAILED(hr)) {
            releaseAudio(grammar, context, recognizer);
            finishListening();
            onEnd(RecognitionEnd::Failed,
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

        releaseAudio(grammar, context, recognizer);
        cancelled = cancelled || m_shuttingDown.load(std::memory_order_acquire)
                    || m_generation.load(std::memory_order_acquire) != generation;
        finishListening();

        if (cancelled) {
            // A stop is a cancellation, not a completed phrase. Reporting the
            // half-heard text here would hand the user a sentence they did not
            // finish as if they had.
            RecognitionResult stopped;
            stopped.isFinal = false;
            stopped.end = RecognitionEnd::Cancelled;
            onResult(stopped);
            onEnd(RecognitionEnd::Cancelled, {});
            return;
        }
        if (!state.failure.empty()) {
            onEnd(RecognitionEnd::Failed, state.failure);
            return;
        }
        if (state.phrase.empty()) {
            onEnd(RecognitionEnd::NoAudio,
                       state.heardSound ? "the microphone heard no recognisable speech"
                                        : "the microphone heard nothing at all");
            return;
        }

        RecognitionResult final;
        final.text = state.phrase;
        final.confidence = state.confidence;
        final.isFinal = true;
        final.end = RecognitionEnd::Silence;
        onResult(final);
        onEnd(RecognitionEnd::Silence, {});
    }

    // Reads whatever the engine has queued. Called on the worker thread with
    // the context it belongs to, so every pointer in here is valid for the
    // duration of the call.
    static void pullEvents(ISpRecoContext* context, SessionState& state) {
        SPEVENT event{};
        ULONG fetched = 0;
        while (SUCCEEDED(context->GetEvents(1, &event, &fetched)) && fetched == 1) {
            if (!state.finished) {
                switch (event.eEventId) {
                case SPEI_SOUND_START:
                    state.heardSound = true;
                    break;
                case SPEI_RECOGNITION: {
                    ISpRecoResult* result = nullptr;
                    if (event.elParamType == SPET_LPARAM_IS_OBJECT && event.lParam != 0
                        && SUCCEEDED(reinterpret_cast<IUnknown*>(event.lParam)
                            ->QueryInterface(IID_PPV_ARGS(&result)))) {
                        wchar_t* text = nullptr;
                        if (SUCCEEDED(result->GetText(SP_GETWHOLEPHRASE, SP_GETWHOLEPHRASE,
                                                       TRUE, &text, nullptr))) {
                            state.phrase = toUtf8(text);
                        }
                        CoTaskMemFree(text);
                        SPPHRASE* phrase = nullptr;
                        if (SUCCEEDED(result->GetPhrase(&phrase)) && phrase != nullptr) {
                            // SAPI's rule confidence is low (-1), normal (0), high (1).
                            state.confidence = std::clamp(
                                (static_cast<double>(phrase->Rule.Confidence) + 1.0) / 2.0,
                                0.0, 1.0);
                        }
                        CoTaskMemFree(phrase);
                        result->Release();
                    }
                    state.finished = true;
                    break;
                }
                case SPEI_FALSE_RECOGNITION:
                    state.phrase.clear();
                    state.finished = true;
                    break;
                default:
                    break;
                }
            }
            clearEvent(event);
        }
    }

    // Deactivate and release the grammar, then the context, then its recognizer.
    // Each COM object is released on the worker that created it.
    static void releaseAudio(ISpRecoGrammar* grammar, ISpRecoContext* context,
                             ISpRecognizer* recognizer) {
        if (grammar != nullptr) {
            grammar->SetDictationState(SPRS_INACTIVE);
            grammar->Release();
        }
        if (context != nullptr) {
            context->SetInterest(0, 0);
            context->Release();
        }
        if (recognizer != nullptr) {
            recognizer->Release();
        }
    }

    /// Dispatches all queued Windows messages on the calling worker thread.
    static void drainMessages() {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
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
    unsigned m_callbackGeneration = 0;
    ResultCallback m_onResult;
    EndCallback m_onEnd;
};

/// Creates the Windows recognition implementation and probes initial availability.
SapiSpeechRecognizer::SapiSpeechRecognizer()
    : m_impl(std::make_unique<Impl>()) {}

/// Destroys the implementation, joining its worker and releasing the wake handle.
SapiSpeechRecognizer::~SapiSpeechRecognizer() = default;

/// Returns the implementation's capture-device and wake-handle availability.
bool SapiSpeechRecognizer::available() const {
    return m_impl->available();
}

/// Returns the Windows adapter's capture-device diagnostic detail.
std::string SapiSpeechRecognizer::detail() const {
    return m_impl->detail();
}

/// Transfers callbacks to the worker implementation and reports whether listening was accepted.
bool SapiSpeechRecognizer::start(ResultCallback onResult, EndCallback onEnd, std::string& error) {
    return m_impl->start(std::move(onResult), std::move(onEnd), error);
}

/// Requests cancellation through the worker implementation without waiting for completion.
void SapiSpeechRecognizer::stop() {
    m_impl->stop();
}

/// Returns whether the implementation currently reports a listening request.
bool SapiSpeechRecognizer::listening() const {
    return m_impl->listening();
}

/// Returns the Windows wave-input capture-device count without opening a device.
unsigned microphoneDeviceCount() {
    return static_cast<unsigned>(waveInGetNumDevs());
}

/// Reports microphone presence from the Windows capture-device count.
Microphone probeMicrophone() {
    return microphoneDeviceCount() > 0 ? Microphone::Present : Microphone::Absent;
}

/// Creates the Windows SAPI speech recognizer adapter.
std::unique_ptr<SpeechRecognizer> makePlatformSpeechRecognizer() {
    return std::make_unique<SapiSpeechRecognizer>();
}

} // namespace kestrel::runtime

#endif // _WIN32
