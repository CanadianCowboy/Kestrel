#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <functional>
#include <memory>

#include "core/voicesession.h"

namespace kestrel::app {

// How long a response may wait for a voice that is installed but not yet able
// to answer, before it is delivered as text instead.
//
// Sized from what the real model does on the machine this was built on. Kokoro's
// driver announces itself 1.31-1.40s after it is started, over five runs, with
// essentially no spread: the 311MB model is in the page cache and the load is
// compute-bound. A cold load is the case that matters, and the model lives
// inside a synced OneDrive folder, so the read is the variable part rather than
// the compute. At a pessimistic 40MB/s that 326MB is 8.2s of I/O on top of the
// measured 1.35s, so a successful cold load is about 9.5s; fifteen seconds
// clears that with roughly 1.5x to spare. Not 30s, because a user waiting half a
// minute for a voice that is never coming has stopped believing the app. Not 10s,
// which would cut off a legitimately slow first load after a re-sync and throw
// away a perfectly good voice.
constexpr int kVoiceLoadDeadlineMs = 15000;

// The platform side of speech, behind an interface.
//
// Everything above this -- which clause comes next, where the cursor moves,
// when the response is delivered -- is platform-independent and worth testing
// on a machine with no voice installed, which is most CI. So the synthesizer
// depends on this rather than on Qt's module directly, and the tests supply an
// implementation that reports utterance boundaries on demand.
class SpeechBackend {
public:
    /// Allows concrete speech backends to be destroyed through the interface.
    virtual ~SpeechBackend() = default;

    [[nodiscard]] virtual bool usable() const = 0;
    // Whether an engine is installed at all, as distinct from whether it can
    // answer yet. A local model is a process: it is on disk before it is ready,
    // so a reply asked for in between has to wait for it rather than be
    // delivered as text. False means there is nothing to wait for, which is the
    // only honest reason to give up on speaking a response. Defaults to usable()
    // for a platform voice, which is simply there or not there.
    [[nodiscard]] virtual bool present() const { return usable(); }
    // Human-readable voice and locale, or why there is not one.
    [[nodiscard]] virtual QString description() const = 0;
    virtual void applyVoice(const core::VoicePersona& persona) = 0;
    virtual void speak(const QString& text) = 0;
    // Asks the engine to have `text` ready before it is asked to speak it. A
    // backend that can start work early does; one that cannot ignores it, which
    // is not an error and must not slow the pump down waiting to find out.
    virtual void prefetch(const QString&) {}
    // Stop at the next utterance boundary: the clause in flight is allowed to
    // finish. This is what makes a barge-in land between clauses.
    virtual void stop() = 0;
    // Stop mid-utterance, for an explicit cancel.
    virtual void stopImmediately() = 0;
    // True while an utterance is in flight, which is what distinguishes "let
    // the clause finish" from "there is nothing to wait for".
    [[nodiscard]] virtual bool speakingNow() const = 0;

    // The voices this backend can speak with, best first, and the one it is
    // currently using. Empty means the choice is not this backend's to make --
    // a platform voice is whatever the operating system registered, and there
    // is nothing useful to offer a list of.
    [[nodiscard]] virtual QStringList voiceChoices() const { return {}; }
    /// Returns no selected voice by default; concrete engines may expose one.
    [[nodiscard]] virtual QString currentVoice() const { return {}; }
    // Returns false when the name is not one this backend has, so a stale
    // setting cannot quietly leave the app speaking with something else.
    virtual bool setVoice(const QString&) { return false; }

    // Called when an utterance reaches its end, and when the engine fails.
    // The backend is handed these at construction rather than reaching back for
    // the synthesizer, so there is no ownership cycle to reason about.
    void setCallbacks(std::function<void()> onFinished,
                      std::function<void(const QString&)> onFailed) {
        m_onFinished = std::move(onFinished);
        m_onFailed = std::move(onFailed);
    }

    // The engine reporting that it can now answer, or that it has given up.
    // A reply that is owed audio waits for one of these rather than being
    // quietly downgraded to text because the check happened too early.
    void setAvailabilityCallbacks(std::function<void()> onAvailable,
                                  std::function<void()> onUnavailable) {
        m_onAvailable = std::move(onAvailable);
        m_onUnavailable = std::move(onUnavailable);
    }

protected:
    /// Invokes the completion callback if one has been installed.
    void reportFinished() const {
        if (m_onFinished) {
            m_onFinished();
        }
    }
    /// Passes a failure reason to the installed callback, if any.
    void reportFailed(const QString& reason) const {
        if (m_onFailed) {
            m_onFailed(reason);
        }
    }

protected:
    // Reported by the engine at the moments its answer actually changes, never
    // polled: a local model becomes ready when its driver says so.
    void reportAvailable() const {
        if (m_onAvailable) {
            m_onAvailable();
        }
    }
    /// Invokes the unavailable callback if one has been installed.
    void reportUnavailable() const {
        if (m_onUnavailable) {
            m_onUnavailable();
        }
    }

private:
    std::function<void()> m_onFinished;
    std::function<void(const QString&)> m_onFailed;
    std::function<void()> m_onAvailable;
    std::function<void()> m_onUnavailable;
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

    // The voices the current backend can speak with, and the one it is using.
    // Empty when the platform voice is in charge, because that choice is the
    // operating system's rather than the app's.
    [[nodiscard]] QStringList voiceChoices() const;
    // Hands the backend the text that is coming next. Separate from speak(),
    // which commits to speaking something now.
    void prefetch(const QString& text);
    // Hands the backend the clause that is coming next, if there is one. Called
    // as soon as the current clause is handed over, so a slow engine can be
    // working on the next sentence while the current one is still being heard.
    void prefetchNext();
    [[nodiscard]] QString currentVoice() const;
    // Changes voice mid-sentence. The clause in flight is not re-spoken: it is
    // already audio, and swapping it out from under the user would be worse
    // than finishing this sentence in the old voice.
    Q_INVOKABLE bool setVoice(const QString& voice);

    // True when a real voice is installed and reachable. False means the
    // response is delivered as text, which is a supported outcome rather than
    // an error.
    [[nodiscard]] bool available() const;

    // Whether a voice is installed but not yet able to answer. True here with
    // available() false is the whole case this exists for: the engine is still
    // coming up, and a response asked for in the meantime is owed audio rather
    // than text.
    [[nodiscard]] bool present() const;

    // True between oweAudio() and releaseOwedAudio(): a response is being held
    // for a voice that is installed and may still arrive. It is owed audio
    // rather than text, and while it is owed the caller has nothing to play and
    // the response has not been completed.
    [[nodiscard]] bool audioOwed() const;

    // Reconciles what a response is owed, at the moment one might need a voice.
    // A voice that can answer now owes nothing; an engine that is installed and
    // still coming up holds the response and starts the load deadline; an
    // engine that has already let us down, or none at all, owes nothing either.
    //
    // The decision is made here rather than at request time because a request
    // time check is the one that was wrong: a local model is on disk and running
    // long before it can answer, so a reply asked for in the first seconds of a
    // session was answered in writing and never revisited.
    void oweAudio();

    // The single way a held response ends without audio: the engine reported it
    // will never answer, the load deadline expired, or the caller withdrew the
    // promise because the user stopped or left the conversation. Safe to call
    // when nothing is held, which is what lets every caller ask for the release
    // rather than track whether one is owed.
    void releaseOwedAudio();

    // Test seam: shortens the load deadline so the give-up path can be driven
    // without waiting out a deadline sized against a real model load.
    void setVoiceLoadTimeout(int ms);

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
    // A voice that was not yet able to answer now can, or never will. Raised by
    // the engine itself, so the controller never has to guess when a response
    // it is holding should be spoken or given up on. A hold has already been
    // ended by the time this arrives with false: owedAudioReleased has said so.
    void availabilityChanged(bool available);
    // A response held for a voice is now text, and the caller is the only thing
    // that can deliver it. voiceGaveUp says the engine was given up on, as
    // opposed to the promise simply being withdrawn, because the two deserve
    // different words in front of the user.
    void owedAudioReleased(bool voiceGaveUp);

private:
    void speakNow();
    void onBackendFinished();
    void onBackendFailed(const QString& reason);
    void onBackendAvailable();
    void onBackendUnavailable();
    void onVoiceLoadDeadline();

    std::unique_ptr<SpeechSynthesizerPrivate> d;
    std::unique_ptr<SpeechBackend> m_backend;
    QTimer m_pauseTimer;
    // Armed only while a response is actually held, so it costs nothing on a
    // machine whose voice is ready when the first question is asked.
    QTimer m_voiceLoadDeadline;
    int m_voiceLoadTimeoutMs = 0;
    bool m_audioOwed = false;
    // A voice was waited for and never came. Until one announces itself, later
    // replies are delivered as text straight away rather than each being held
    // for the same engine that already let us down.
    bool m_voiceGaveUp = false;
    QString m_pending;
    QString m_pendingText;
    // A lookahead waiting for the clause in front of it to be committed. Kept
    // rather than sent immediately so the engine is asked for clauses in the
    // order they are spoken; see prefetch().
    QString m_queuedPrefetch;
    bool m_stoppingAtBoundary = false;
};

} // namespace kestrel::app
