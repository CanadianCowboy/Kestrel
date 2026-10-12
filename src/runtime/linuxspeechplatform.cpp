#include "runtime/linuxspeechrecognizer.h"
#include "runtime/sapirecognizer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <thread>

#if KESTREL_HAS_LINUX_SPEECH
#include <alsa/asoundlib.h>
#include <whisper.h>
#endif

namespace kestrel::runtime {
namespace {

std::string environment(const char* name, const char* fallback = "") {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? value : fallback;
}

#if KESTREL_HAS_LINUX_SPEECH

class AlsaSpeechCapture final : public SpeechCapture {
public:
    explicit AlsaSpeechCapture(std::string device) : m_device(std::move(device)) {}

    bool capture(std::vector<float>& samples, const SpeechCancelled& cancelled,
                 std::string& error) override {
        samples.clear();
        error.clear();
        snd_pcm_t* raw = nullptr;
        int status = snd_pcm_open(&raw, m_device.c_str(), SND_PCM_STREAM_CAPTURE, SND_PCM_NONBLOCK);
        if (status < 0) {
            error = "Could not open the ALSA microphone: " + std::string(snd_strerror(status))
                + ". Check microphone permissions and the ALSA device in Dictation settings or KESTREL_AUDIO_DEVICE (default: default).";
            return false;
        }
        const std::unique_ptr<snd_pcm_t, decltype(&snd_pcm_close)> pcm(raw, snd_pcm_close);
        status = snd_pcm_set_params(raw, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                    1, 16000, 1, 100000);
        if (status < 0) {
            error = "The microphone could not supply mono 16 kHz PCM: "
                + std::string(snd_strerror(status)) + ". Select another ALSA device.";
            return false;
        }
        // At most 15 seconds of PCM, in memory only. Silence endpointing is
        // deliberately simple RMS gating, not a claim of full-duplex VAD/AEC.
        std::array<short, 320> block{};
        std::vector<float> lead;
        std::size_t voiced = 0;
        std::size_t quiet = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (!cancelled() && std::chrono::steady_clock::now() < deadline) {
            const snd_pcm_sframes_t count = snd_pcm_readi(raw, block.data(), block.size());
            if (count == -EAGAIN || count == 0) {
                // Nonblocking capture makes cancellation independent of a
                // broken audio server that never returns another sample.
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (count == -EPIPE || count == -ESTRPIPE) {
                error = "Microphone capture was interrupted. Check the audio service and try again.";
                return false;
            }
            if (count < 0) {
                error = "Microphone capture failed: " + std::string(snd_strerror(static_cast<int>(count)));
                return false;
            }
            double energy = 0;
            for (snd_pcm_sframes_t i = 0; i < count; ++i) {
                const float value = static_cast<float>(block[static_cast<std::size_t>(i)]) / 32768.0F;
                energy += static_cast<double>(value) * value;
                lead.push_back(value);
            }
            const bool speech = std::sqrt(energy / static_cast<double>(count)) >= 0.01;
            if (speech) {
                voiced += static_cast<std::size_t>(count);
                quiet = 0;
            } else {
                quiet += static_cast<std::size_t>(count);
            }
            if (voiced > 0) {
                samples.insert(samples.end(), lead.begin(), lead.end());
                lead.clear();
                if (voiced >= 4800 && quiet >= 14400) {
                    return true; // >=300ms speech followed by >=900ms silence
                }
                if (samples.size() >= 240000) {
                    if (voiced < 4800) { samples.clear(); }
                    return true;
                }
            } else if (lead.size() > 3200) {
                lead.erase(lead.begin(), lead.end() - 3200); // 200ms pre-roll
            }
        }
        if (cancelled()) {
            return false;
        }
        if (voiced < 4800) {
            samples.clear(); // Never ask Whisper to hallucinate words from silence.
        }
        return true;
    }
private:
    std::string m_device;
};

class WhisperTranscriber final : public SpeechTranscriber {
public:
    WhisperTranscriber(std::string model, std::string language)
        : m_model(std::move(model)), m_language(std::move(language)) {}
    ~WhisperTranscriber() override {
        if (m_context != nullptr) {
            whisper_free(m_context);
        }
    }

    bool transcribe(const std::vector<float>& samples, const SpeechCancelled& cancelled,
                    std::string& text, std::string& error) override {
        text.clear();
        error.clear();
        if (cancelled()) {
            return false;
        }
        if (samples.empty() || samples.size() > 30 * 16000) {
            error = "Whisper needs between one sample and 30 seconds of mono 16 kHz audio.";
            return false;
        }
        if (whisper_lang_id(m_language.c_str()) < 0) {
            error = "Unknown Whisper language. Set KESTREL_WHISPER_LANGUAGE to a language code such as en.";
            return false;
        }
        if (m_context == nullptr) {
            static std::once_flag loaded;
            std::call_once(loaded, [] { ggml_backend_load_all(); });
            whisper_context_params parameters = whisper_context_default_params();
            parameters.use_gpu = false; // Reliable CPU dictation; no GPU contention with chat.
            m_context = whisper_init_from_file_with_params(m_model.c_str(), parameters);
            if (m_context == nullptr) {
                error = "Could not load the local Whisper model. Check KESTREL_WHISPER_MODEL, the model format and available RAM.";
                return false;
            }
        }
        if (cancelled()) {
            return false;
        }
        whisper_full_params parameters = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        parameters.n_threads = static_cast<int>(std::clamp(std::thread::hardware_concurrency(), 1u, 4u));
        parameters.language = m_language.c_str();
        parameters.translate = false;
        parameters.no_context = true;
        parameters.no_timestamps = true;
        parameters.print_special = false;
        parameters.print_progress = false;
        parameters.print_realtime = false;
        parameters.print_timestamps = false;
        parameters.abort_callback = [](void* data) {
            return (*static_cast<const SpeechCancelled*>(data))();
        };
        parameters.abort_callback_user_data = const_cast<SpeechCancelled*>(&cancelled);
        parameters.encoder_begin_callback = [](whisper_context*, whisper_state*, void* data) {
            return !(*static_cast<const SpeechCancelled*>(data))();
        };
        parameters.encoder_begin_callback_user_data = const_cast<SpeechCancelled*>(&cancelled);
        const int status = whisper_full(m_context, parameters, samples.data(), static_cast<int>(samples.size()));
        if (cancelled()) {
            return false;
        }
        if (status != 0) {
            error = "Whisper could not transcribe this audio. Check the model and available RAM, then retry.";
            return false;
        }
        for (int i = 0; i < whisper_full_n_segments(m_context); ++i) {
            text += whisper_full_get_segment_text(m_context, i);
        }
        return true;
    }
private:
    std::string m_model;
    std::string m_language;
    whisper_context* m_context = nullptr;
};
#endif

} // namespace

std::unique_ptr<SpeechTranscriber> makeWhisperTranscriber(std::string modelPath, std::string language) {
#if KESTREL_HAS_LINUX_SPEECH
    return std::make_unique<WhisperTranscriber>(std::move(modelPath), std::move(language));
#else
    static_cast<void>(modelPath);
    static_cast<void>(language);
    return nullptr;
#endif
}

std::unique_ptr<SpeechCapture> makeAlsaSpeechCapture(std::string device) {
#if KESTREL_HAS_LINUX_SPEECH
    return std::make_unique<AlsaSpeechCapture>(std::move(device));
#else
    static_cast<void>(device);
    return nullptr;
#endif
}

std::unique_ptr<SpeechRecognizer> makeLinuxSpeechRecognizer() {
    return makeLinuxSpeechRecognizer(environment("KESTREL_WHISPER_MODEL"),
        environment("KESTREL_AUDIO_DEVICE", "default"), environment("KESTREL_WHISPER_LANGUAGE", "en"));
}

std::unique_ptr<SpeechRecognizer> makeLinuxSpeechRecognizer(
    std::string model, std::string device, std::string language) {
    std::string reason;
#if KESTREL_HAS_LINUX_SPEECH
    std::error_code code;
    if (model.empty() || !std::filesystem::is_regular_file(model, code)) {
        reason = "Offline dictation needs a local Whisper model. Choose a ggml model file in Dictation settings or set KESTREL_WHISPER_MODEL. Type your message instead.";
    }
#else
    reason = "Offline Linux dictation was not compiled in. Install whisper.cpp and ALSA development libraries and reconfigure with KESTREL_ENABLE_LINUX_SPEECH=ON. Type your message instead.";
#endif
    return std::make_unique<LinuxSpeechRecognizer>(
        [device = std::move(device)] { return makeAlsaSpeechCapture(device); },
        [model = std::move(model), language = std::move(language)] {
            return makeWhisperTranscriber(model, language);
        }, reason);
}

std::vector<std::string> linuxMicrophoneDevices() {
    std::vector<std::string> devices;
#if KESTREL_HAS_LINUX_SPEECH
    // Enumerate capture hints, never open a microphone during startup/probing.
    void** hints = nullptr;
    if (snd_device_name_hint(-1, "pcm", &hints) < 0) {
        return devices;
    }
    for (void** hint = hints; *hint != nullptr; ++hint) {
        char* name = snd_device_name_get_hint(*hint, "NAME");
        char* direction = snd_device_name_get_hint(*hint, "IOID");
        if (name != nullptr && std::string_view(name) != "null"
            && (direction == nullptr || std::string_view(direction) != "Output")) {
            if (std::find(devices.begin(), devices.end(), name) == devices.end()) {
                devices.emplace_back(name);
            }
        }
        std::free(name);
        std::free(direction);
    }
    snd_device_name_free_hint(hints);
#endif
    return devices;
}

unsigned microphoneDeviceCount() {
#if KESTREL_HAS_LINUX_SPEECH
    return static_cast<unsigned>(linuxMicrophoneDevices().size());
#else
    return 0;
#endif
}

Microphone probeMicrophone() {
    return microphoneDeviceCount() > 0 ? Microphone::Present : Microphone::Absent;
}

std::unique_ptr<SpeechRecognizer> makePlatformSpeechRecognizer() {
    return makeLinuxSpeechRecognizer();
}

} // namespace kestrel::runtime
