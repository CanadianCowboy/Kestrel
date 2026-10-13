#pragma once

#include "runtime/speechrecognizer.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace kestrel::runtime {

// Audio is mono, normalized float PCM at 16 kHz. No recordings are written to
// disk. Factories are called on the recognizer's worker, never on the UI thread.
using SpeechCancelled = std::function<bool()>;

class SpeechCapture {
public:
    virtual ~SpeechCapture() = default;
    virtual bool capture(std::vector<float>& samples, const SpeechCancelled& cancelled,
                         std::string& error) = 0;
};

class SpeechTranscriber {
public:
    virtual ~SpeechTranscriber() = default;
    virtual bool transcribe(const std::vector<float>& samples, const SpeechCancelled& cancelled,
                            std::string& text, std::string& error) = 0;
};

class LinuxSpeechRecognizer final : public SpeechRecognizer {
public:
    using CaptureFactory = std::function<std::unique_ptr<SpeechCapture>()>;
    using TranscriberFactory = std::function<std::unique_ptr<SpeechTranscriber>()>;

    LinuxSpeechRecognizer(CaptureFactory capture, TranscriberFactory transcriber,
                          std::string unavailableReason = {});
    ~LinuxSpeechRecognizer() override;
    bool available() const override;
    std::string detail() const override;
    bool start(ResultCallback onResult, EndCallback onEnd, std::string& error) override;
    void stop() override;
    void stopAndWait() override;
    bool listening() const override;
    // A cancelled session may still be unwinding on the worker.
    [[nodiscard]] bool idle() const;

private:
    class Impl;
    std::unique_ptr<Impl> m_impl;
};

// May return an unavailable adapter when SDKs/model are absent. Model and
// language are explicit; inference never downloads weights or contacts a service.
[[nodiscard]] std::unique_ptr<SpeechTranscriber> makeWhisperTranscriber(
    std::string modelPath, std::string language = "en");
[[nodiscard]] std::unique_ptr<SpeechCapture> makeAlsaSpeechCapture(std::string device = "default");
[[nodiscard]] std::unique_ptr<SpeechRecognizer> makeLinuxSpeechRecognizer();
[[nodiscard]] std::unique_ptr<SpeechRecognizer> makeLinuxSpeechRecognizer(
    std::string modelPath, std::string device, std::string language);
// Capture device hints only: enumeration never opens an audio stream.
[[nodiscard]] std::vector<std::string> linuxMicrophoneDevices();

} // namespace kestrel::runtime
