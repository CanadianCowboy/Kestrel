#pragma once

#include "app/speechsynthesizer.h"

#include <QHash>
#include <QList>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QMediaPlayer;
class QAudioOutput;
class QProcess;
class QTemporaryDir;

namespace kestrel::app {

// The voice Windows does not have.
//
// Everything else in this app speaks through Qt's text-to-speech, which is
// limited to whatever voices the machine has registered -- on a stock English
// install that is three recordings from the early 2000s, and they are the
// reason an assistant sounds like a navigation system rather than a
// colleague. No better voice can be installed as a system voice, because none
// of the good ones are SAPI engines.
//
// This backend sidesteps that. It drives a local Kokoro model over a pipe and
// plays the audio itself, which means the same machine that would otherwise
// report "Microsoft David" can speak in a voice that sounds like a person.
// Kokoro is 82 million parameters, runs on the CPU faster than real time, and
// is Apache-2.0.
//
// It is a SpeechBackend like any other, so the clause pacing, barge-in, and
// delivery rules above it do not change: it is asked to speak one clause, and
// it says when that clause has finished. Choosing between this and the platform
// voice is the synthesizer's job, not anybody else's.
// QObject comes first: the meta-object system requires it to be the primary
// base, and without it this class cannot have slots or be tracked by Qt at all.
class KokoroSpeechBackend final : public QObject, public SpeechBackend {
    Q_OBJECT

public:
    // `python` is the interpreter that has kokoro-onnx installed and
    // `serverScript` the driver. Both are discovered rather than hardcoded, so a
    // developer can point at their own environment and a machine without one
    // simply reports that it has no voice.
    KokoroSpeechBackend(QString python, QString serverScript, QObject* parent = nullptr);
    ~KokoroSpeechBackend() override;

    // True once the model has loaded and answered. The first clause cannot be
    // spoken before then, so the synthesizer waits for this rather than
    // discovering it by failing.
    [[nodiscard]] bool usable() const override;
    [[nodiscard]] QString description() const override;

    void applyVoice(const core::VoicePersona& persona) override;
    void speak(const QString& text) override;
    // Synthesises ahead of time and remembers the result, so speak() is usually
    // a file read rather than a wait. This is what removes the gap between one
    // sentence and the next.
    void prefetch(const QString& text) override;
    void stop() override;
    void stopImmediately() override;
    [[nodiscard]] bool speakingNow() const override;

    // Kokoro names its voices af_*/am_*/bf_*/bm_*. The British male set suits an
    // assistant better than the American one, which is why it is the default.
    [[nodiscard]] QStringList voiceChoices() const override;
    [[nodiscard]] QString currentVoice() const override { return m_voice; }
    bool setVoice(const QString& voice) override;

private:
    void onReadyRead();
    // Text already synthesised, waiting to be played.
    [[nodiscard]] QString takePrefetched(const QString& text);
    void requestSynthesis(const QString& text, const QString& path, bool playsNow);
    void onProcessError(QProcess::ProcessError error);
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onMediaStatusChanged();
    void failWith(const QString& reason);

    QString m_python;
    QString m_serverScript;
    QString m_voice;
    double m_speed = 1.0;
    // The process is up and accepting requests. Separate from m_ready, which
    // means the model has finished loading: a request sent in between is
    // queued by the server rather than lost.
    bool m_started = false;
    bool m_ready = false;
    bool m_speaking = false;
    int m_clause = 0;

    QProcess* m_process = nullptr;
    QHash<QString, QString> m_prefetched;
    // What is outstanding on the pipe. Two can be at once: the clause being
    // spoken and the one the caller warned about while it was being asked for.
    // The server answers in the order it was written, so replies are matched to
    // requests by taking the front -- which is only correct if this really is a
    // queue and not a single slot.
    struct PendingRequest {
        QString text;
        bool playsNow = false;
    };
    QList<PendingRequest> m_pending;
    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audio = nullptr;
    std::unique_ptr<QTemporaryDir> m_scratch;
};

} // namespace kestrel::app
