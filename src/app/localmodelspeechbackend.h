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

class QAudioOutput;
class QMediaPlayer;
class QTemporaryDir;

namespace kestrel::app {

// The shared half of a locally-run speech model.
//
// Kokoro and Piper are different models in different runtimes, but from this
// app's point of view they are the same thing: hand one a clause, get a WAV,
// play it, and be told when it has finished. Everything that follows from that
// -- the scratch file, the player, the request queue, the prefetch cache, the
// release order before a callback -- is identical, and having it in one place is
// what keeps the two engines from drifting into behaving differently in ways
// nobody notices until a reply is delivered at the wrong moment.
//
// A subclass supplies four things: how to start the engine, how to ask it for a
// WAV, which voices it has, and what it is called.
class LocalModelSpeechBackend : public QObject, public SpeechBackend {
    Q_OBJECT

public:
    ~LocalModelSpeechBackend() override;

    [[nodiscard]] bool usable() const override;
    // The engine is on disk and has been launched, whether or not its model has
    // finished loading. This is what tells the controller there is something to
    // wait for, so a reply asked for during the load is spoken rather than
    // downgraded to text.
    [[nodiscard]] bool present() const override { return m_launched; }
    [[nodiscard]] QString description() const override;

    void applyVoice(const core::VoicePersona& persona) override;
    void speak(const QString& text) override;
    void prefetch(const QString& text) override;
    void stop() override;
    void stopImmediately() override;
    [[nodiscard]] bool speakingNow() const override;

    /// Returns the concrete engine's supported voice identifiers.
    [[nodiscard]] QStringList voiceChoices() const override { return engineVoices(); }
    /// Returns the selected voice identifier.
    [[nodiscard]] QString currentVoice() const override { return m_voice; }
    bool setVoice(const QString& voice) override;

protected:
    explicit LocalModelSpeechBackend(QObject* parent = nullptr);
    // Finishes construction, and must be called at the end of a derived
    // constructor. The base cannot do it itself: calling a pure virtual from the
    // base constructor dispatches through the base's vtable, which does not yet
    // know which engine it is starting.
    void completeSetup();

    // Brings the engine up. Returning without starting it leaves the backend
    // reporting unavailable, which is the correct outcome for a machine that
    // has not installed the model.
    virtual void startEngine() = 0;
    // Queues `text` to be written to `path`. Must not block: the reply is
    // delivered later through noteEngineReady or noteEngineFailed.
    virtual void synthesise(const QString& text, const QString& path) = 0;
    [[nodiscard]] virtual QStringList engineVoices() const = 0;
    [[nodiscard]] virtual QString engineName() const = 0;
    // An engine that needs an extra argument per request, such as a path to a
    // model file. Null when the engine is told everything at start-up.
    [[nodiscard]] virtual QString engineArgument() const { return {}; }
    // A WAV has been produced for `path`. The base decides whether that means
    // "play it now" or "hold it for when it is asked for".
    void noteSynthesised(const QString& path);
    void noteEngineFailed(const QString& reason);
    // One clause could not be produced. The engine keeps going: a single
    // sentence failing should not silence the rest of the reply, and the reason
    // is reported rather than swallowed.
    void noteClauseFailed(const QString& reason);
    // Called once the driver process is up. Requests written to it from here on
    // are answered, just not immediately: the driver queues them while the model
    // loads. This is what makes present() true, and it is deliberately not the
    // same moment as markStarted().
    void markLaunched() { m_launched = true; }
    // Called once the engine has answered for itself and can actually produce
    // audio. Deliberately not the moment the process spawns: a local model is
    // hundreds of megabytes loaded after its interpreter is already running, and
    // reporting a voice before then is a claim nothing can back up.
    void markStarted() {
        if (m_started) {
            return;
        }
        m_started = true;
        reportAvailable();
    }
    // The engine will not be answering. Says so, so anything waiting on it stops
    // waiting rather than holding a response that is never going to be spoken.
    void markGivenUp();
    // The persona's speaking rate, as an engine should be told it.
    [[nodiscard]] double speedForRequest() const { return m_speed; }

    [[nodiscard]] QString engineExecutable() const;
    // Reports that the engine gave up before it ever started. Safe to call when
    // it never could: the answer is the same either way.
    void failIfNotStarted() { markGivenUp(); }

private:
    void onMediaStatusChanged();
    [[nodiscard]] QString takePrefetched(const QString& text);
    [[nodiscard]] QString nextScratchPath();

    // What is outstanding. Two can be at once: the clause being spoken and the
    // one the caller warned about while it was being asked for. The engines
    // answer in the order they were written to, so replies are matched to
    // requests by taking the front -- correct only because this is a queue and
    // not a single slot.
    struct PendingRequest {
        QString text;
        bool playsNow = false;
        bool obsolete = false;
    };
    QList<PendingRequest> m_pending;
    QHash<QString, QString> m_prefetched;

    QString m_voice;
    double m_speed = 1.0;
    // Launched: the driver process is up. Not the same as started, and not
    // enough to take work -- nothing reaches this class until the engine has
    // announced itself, because the synthesizer will not hand it a clause before
    // then. It exists so present() can say a voice is coming.
    bool m_launched = false;
    // Started: the engine has confirmed it can produce audio now.
    bool m_started = false;
    bool m_speaking = false;
    int m_clause = 0;

    QMediaPlayer* m_player = nullptr;
    QAudioOutput* m_audio = nullptr;
    std::unique_ptr<QTemporaryDir> m_scratch;
};

} // namespace kestrel::app
