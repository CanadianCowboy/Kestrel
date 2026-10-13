#include "runtime/linuxspeechrecognizer.h"

#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace kestrel::runtime {

class LinuxSpeechRecognizer::Impl {
public:
    Impl(CaptureFactory captureFactory, TranscriberFactory transcriberFactory, std::string reason)
        : capture(std::move(captureFactory)), transcriber(std::move(transcriberFactory)),
          unavailableReason(std::move(reason)), worker([this] { run(); }) {}

    ~Impl() {
        {
            std::lock_guard lock(mutex);
            shuttingDown = true;
            ++generation;
            active = false;
            pending = false;
        }
        wake.notify_all();
        worker.join();
    }

    void run() {
        std::unique_ptr<SpeechCapture> input;
        std::unique_ptr<SpeechTranscriber> decoder;
        for (;;) {
            ResultCallback result;
            EndCallback end;
            unsigned long long session = 0;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [this] { return shuttingDown || pending; });
                if (shuttingDown) {
                    return;
                }
                session = generation;
                result = std::move(onResult);
                end = std::move(onEnd);
                pending = false;
                busy = true;
            }
            const SpeechCancelled cancelled = [this, session] {
                return generation.load() != session;
            };
            RecognitionEnd reason = RecognitionEnd::Failed;
            std::string text;
            std::string error;
            try {
                if (!input) {
                    input = capture();
                }
                if (!decoder) {
                    decoder = transcriber();
                }
                std::vector<float> samples;
                if (!input || !decoder) {
                    error = "Linux speech components could not be created. Check the audio and Whisper setup.";
                } else if (input->capture(samples, cancelled, error) && !cancelled()) {
                    if (samples.empty()) {
                        reason = RecognitionEnd::NoAudio;
                        error = "No speech was detected. Check the microphone and try again.";
                    } else if (decoder->transcribe(samples, cancelled, text, error)) {
                        const auto first = text.find_first_not_of(" \t\r\n");
                        text = first == std::string::npos ? std::string()
                            : text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
                        reason = text.empty() ? RecognitionEnd::NoAudio : RecognitionEnd::Silence;
                        if (text.empty()) {
                            error = "No words were recognized. Try again closer to the microphone.";
                        }
                    }
                }
            } catch (const std::exception&) {
                error = "Linux dictation failed while loading or processing audio. Check available RAM and the model.";
            }
            if (!cancelled()) {
                if (reason == RecognitionEnd::Silence) {
                    RecognitionResult phrase;
                    phrase.text = std::move(text);
                    phrase.isFinal = true;
                    phrase.end = reason;
                    // Whisper does not expose a calibrated phrase confidence.
                    result(phrase);
                }
                if (!cancelled()) {
                    end(reason, error);
                }
            }
            {
                std::lock_guard lock(mutex);
                if (generation == session) {
                    active = false;
                }
                busy = false;
            }
            idle.notify_all();
        }
    }

    CaptureFactory capture;
    TranscriberFactory transcriber;
    const std::string unavailableReason;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable idle;
    std::atomic<unsigned long long> generation{0};
    std::atomic<bool> active{false};
    bool shuttingDown = false;
    bool pending = false;
    bool busy = false;
    ResultCallback onResult;
    EndCallback onEnd;
    std::thread worker;
};

LinuxSpeechRecognizer::LinuxSpeechRecognizer(CaptureFactory capture, TranscriberFactory transcriber,
                                             std::string unavailableReason)
    : m_impl(std::make_unique<Impl>(std::move(capture), std::move(transcriber),
                                    std::move(unavailableReason))) {}
LinuxSpeechRecognizer::~LinuxSpeechRecognizer() = default;

bool LinuxSpeechRecognizer::available() const {
    return m_impl->unavailableReason.empty() && m_impl->capture && m_impl->transcriber;
}

std::string LinuxSpeechRecognizer::detail() const {
    if (available()) {
        return "Offline whisper.cpp dictation with ALSA capture; completed phrases only";
    }
    return m_impl->unavailableReason.empty()
        ? "Linux dictation components are unavailable. Check the Whisper and ALSA installation."
        : m_impl->unavailableReason;
}

bool LinuxSpeechRecognizer::start(ResultCallback onResult, EndCallback onEnd, std::string& error) {
    error.clear();
    if (!available()) {
        error = detail();
        return false;
    }
    if (!onResult || !onEnd) {
        error = "Dictation needs both result and end callbacks.";
        return false;
    }
    {
        std::lock_guard lock(m_impl->mutex);
        if (m_impl->active) {
            error = "Dictation is already listening.";
            return false;
        }
        ++m_impl->generation;
        m_impl->onResult = std::move(onResult);
        m_impl->onEnd = std::move(onEnd);
        m_impl->active = true;
        m_impl->pending = true;
    }
    m_impl->wake.notify_one();
    return true;
}

void LinuxSpeechRecognizer::stop() {
    std::lock_guard lock(m_impl->mutex);
    ++m_impl->generation;
    m_impl->active = false;
    m_impl->pending = false;
    m_impl->onResult = {};
    m_impl->onEnd = {};
}

void LinuxSpeechRecognizer::stopAndWait() {
    stop();
    std::unique_lock lock(m_impl->mutex);
    m_impl->idle.wait(lock, [this] { return !m_impl->busy; });
}

bool LinuxSpeechRecognizer::listening() const { return m_impl->active; }

bool LinuxSpeechRecognizer::idle() const {
    std::lock_guard lock(m_impl->mutex);
    return !m_impl->active && !m_impl->busy && !m_impl->pending;
}

} // namespace kestrel::runtime
