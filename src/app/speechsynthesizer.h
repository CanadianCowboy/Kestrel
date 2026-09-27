#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

#include "core/voicesession.h"

namespace kestrel::app {

// The platform side of speech, behind an interface.
//
// Everything above this -- which clause comes next, where the cursor moves,
// when the response is delivered -- is platform-independent and worth testing
// on a machine with no voice installed, which is most CI. So the synthesizer
// depends on this rather than on Qt's module directly, and the tests supply an
// implementation that reports utterance boundaries on demand.
class SpeechBackend {
public:
    virtual ~SpeechBackend() = default;

    [[nodiscard]] virtual bool usable() const = 0;
    // Human-readable voice and locale, or why there is not one.
    [[nodiscard]] virtual QString description() const = 0;
    virtual void applyVoice(const core::VoicePersona& persona) = 0;
    virtual void speak(const QString& text) = 0;
    // Stop at the next utterance boundary: the clause in flight is allowed to
    // finish. This is what makes a barge-in land between clauses.
    virtual void stop() = 0;
    // Stop mid-utterance, for an explicit cancel.
    virtual void stopImmediately() = 0;
    // True while an utterance is in flight, which is what distinguishes "let
    // the clause finish" from "there is nothing to wait for".
    [[nodiscard]] virtual bool speakingNow() const = 0;

    // Called when an utterance reaches its end, and when the engine fails.
    // The backend is handed these at construction rather than reaching back for
    // the synthesizer, so there is no ownership cycle to reason about.
    void setCallbacks(std::function<void()> onFinished,
                      std::function<void(const QString&)> onFailed) {
        m_onFinished = std::move(onFinished);
        m_onFailed = std::move(onFailed);
    }

protected:
    void reportFinished() const {
        if (m_onFinished) {
            m_onFinished();
        }
    }
    void reportFailed(const QString& reason) const {
        if (m_onFailed) {
            m_onFailed(reason);
        }
    }

private:
    std::function<void()> m_onFinished;
    std::function<void(const QString&)> m_onFailed;
};

class SpeechSynthesizerPrivate;

// Speaks a response as it is generated, one clause at a time.
//
// Two decisions live here rather than in QML. The first is what to say: the
// segments come from core::VoiceSession, which owns the clause boundaries and
// the pauses, so the interface never has to know how a sentence is split. The
// second is how to stop, which is the whole reason a response is spoken clause
// by clause. A user who starts talking should not cut a reply off mid-word;
// requestStop() lets the clause in flight finish and then goes quiet, while
// stopNow() cuts immediately for an explicit cancel.
//
// The synthesizer is optional. Qt's text-to-speech module may not be present in
// a contributor's Qt build and the machine may have no installed voice, in
// which case available() is false and the caller keeps the text-only path. That
// is the same degrade-gracefully rule the runtime backends follow, and it is why
// nothing in the app may assume audio exists.
class SpeechSynthesizer final : public QObject {
    Q_OBJECT

public:
    explicit SpeechSynthesizer(QObject* parent = nullptr);
    ~SpeechSynthesizer() override;

    // Test seam: adopt a backend supplied by the caller instead of the platform
    // one, so the clause pump can be driven on a machine with no voice. Mirrors
    // AppController::setBackendForTesting. Ownership transfers; the previous
    // backend is dropped first.
    void setBackendForTesting(std::unique_ptr<SpeechBackend> backend);

    // Takes ownership of a backend and uses it in place of the platform one.
    // This is the production counterpart of the seam above: a machine with a
    // better local voice installed hands one over, rather than being limited to
    // whatever voices the operating system happened to register.
    void adoptBackend(std::unique_ptr<SpeechBackend> backend);

    // True when a real voice is installed and reachable. False means the
    // response is delivered as text, which is a supported outcome rather than
    // an error.
    [[nodiscard]] bool available() const;

    // Human-readable voice and locale, for the diagnostics panel.
    [[nodiscard]] QString voiceDescription() const;

    // Applies rate, pitch, and volume from the core voice persona, so the
    // pacing the timeline was planned with is the pacing that gets spoken.
    void applyVoice(const core::VoicePersona& persona);

    // Speaks one clause, waiting `leadingPauseMs` first. The pause belongs to
    // the segment, which is what makes a gap after an acknowledgement cue, or
    // between two clauses, deliberate rather than accidental.
    void speak(const QString& text, int leadingPauseMs = 0);

    // Finishes the clause in flight, then stops. The controller stops feeding
    // segments on segmentFinished, so this is barge-in at a clause boundary.
    void requestStop();

    // Cuts off immediately. For an explicit cancel, where waiting out the
    // current clause would feel like the app ignored the button.
    void stopNow();

    [[nodiscard]] bool speaking() const noexcept;
    [[nodiscard]] bool waiting() const noexcept;

signals:
    // A queued clause finished speaking. The controller answers by asking
    // VoiceSession for the next one, or by letting the response complete.
    void segmentFinished();
    // Playback ended for any reason: drained, cut off, or stopped at a clause
    // boundary. The response's own state says which.
    void stopCompleted();
    // The synthesizer hit a real error, e.g. the voice disappeared mid-session.
    void failed(const QString& reason);

private:
    void speakNow();
    void onBackendFinished();
    void onBackendFailed(const QString& reason);

    std::unique_ptr<SpeechSynthesizerPrivate> d;
    std::unique_ptr<SpeechBackend> m_backend;
    QTimer m_pauseTimer;
    QString m_pending;
    QString m_pendingText;
    bool m_stoppingAtBoundary = false;
};

} // namespace kestrel::app
