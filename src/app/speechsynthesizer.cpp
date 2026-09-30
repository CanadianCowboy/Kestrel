#include "app/speechsynthesizer.h"

#include <QMetaObject>

#include <algorithm>

#if KESTREL_HAS_TEXT_TO_SPEECH
#include <QLocale>
#include <QStringList>
#include <QTextToSpeech>
#include <QVoice>
#endif

namespace kestrel::app {

namespace {

// Longest gap between clauses. A pause longer than this is not pacing, it is a
// hang, and clamping keeps one odd plan from stalling the whole response.
constexpr int kMaxPauseMs = 1200;

#if KESTREL_HAS_TEXT_TO_SPEECH

// Map an octave in either direction onto Qt's normalized pitch range.
constexpr double kPitchPerSemitone = 1.0 / 12.0;

// Choosing which voice to speak with.
//
// A machine usually has several, and they are not equally good: the ones that
// ship with Windows are decades-old concatenative recordings that were
// intelligible rather than pleasant, and they are not updated the way software
// is. The neural engines that sound like a person -- Kokoro or Piper registered
// as system voices, which is how a Windows user would install one -- are a single
// download away and are usually far better than anything already present.
//
// So the first voice in the list is not a safe default. Alphabetically, on a
// stock English install, it is "Microsoft David", which is exactly the voice
// that makes an assistant sound like a 2003 navigation system. The rules below
// are about preferring a better voice when the machine has one, and degrading to
// a pleasant one when it does not -- never about refusing to speak.

// Words in a voice's name that mark it as a neural or recorded voice rather than
// a legacy synthesiser. Matched loosely because every project names its own.
bool soundsLikeNeuralVoice(const QString& name) {
    static const QStringList kMarkers = {
        QStringLiteral("kokoro"),   QStringLiteral("piper"),    QStringLiteral("neural"),
        QStringLiteral("natural"),  QStringLiteral("onnx"),     QStringLiteral("eleven"),
        QStringLiteral("azure"),    QStringLiteral("sonify"),   QStringLiteral("alloy"),
        QStringLiteral("jenny"),    QStringLiteral("guy"),      QStringLiteral("aria"),
    };
    const QString lowered = name.toLower();
    for (const QString& marker : kMarkers) {
        if (lowered.contains(marker)) {
            return true;
        }
    }
    return false;
}

// Windows marks its thin legacy voices by naming them "Compact" in the voice's
// own name, which is the only hint available about how one actually sounds. The
// compact set is intelligible rather than pleasant, and it is what a machine
// falls back to when nothing better is installed.
bool soundsStrained(const QVoice& voice) {
    const QString name = voice.name().toLower();
    return name.contains(QStringLiteral("compact"))
        || name.contains(QStringLiteral("whisper"))
        || name.contains(QStringLiteral("babble"));
}

/// Ranks language matches first, then neural-sounding and unstrained voices.
int scoreVoice(const QVoice& voice, const QLocale& preferred) {
    int score = soundsLikeNeuralVoice(voice.name()) ? 30 : 0;
    // A voice the user can actually understand matters more than one that merely
    // exists, so the language match is checked before the niceness of the name.
    if (voice.locale().language() == preferred.language()) {
        score += 100;
    }
    if (!soundsStrained(voice)) {
        score += 20;
    }
    return score;
}

// The best voice available, or the first one when they are all the same. Ties
// keep the platform's own order, because there is no better information and
// inventing a preference here would be noise.
QVoice chooseVoice(const QList<QVoice>& voices, const QLocale& preferred) {
    QVoice best = voices.first();
    int bestScore = scoreVoice(best, preferred);
    for (const QVoice& voice : voices) {
        const int score = scoreVoice(voice, preferred);
        if (score > bestScore) {
            best = voice;
            bestScore = score;
        }
    }
    return best;
}

#endif // KESTREL_HAS_TEXT_TO_SPEECH

} // namespace

#if KESTREL_HAS_TEXT_TO_SPEECH
// The platform voice. This translation unit is the only place that knows the
// module exists, which is what lets the app build and behave identically
// without it.
class PlatformSpeechBackend final : public SpeechBackend {
public:
    /// Returns the platform voice availability recorded during synthesizer setup.
    bool usable() const override { return m_usable; }
    /// Returns the selected platform voice name or its availability explanation.
    QString description() const override { return m_description; }

    // The synthesizer decides once whether an engine answered and records the
    // answer here. Without a setter the flags below would keep their defaults
    // forever and a perfectly good voice would read as absent.
    void setAvailability(bool usable, const QString& description) {
        m_usable = usable;
        m_description = description;
    }

    /// Applies persona rate and pitch to a usable platform voice, leaving volume unchanged.
    void applyVoice(const core::VoicePersona& persona) override {
        if (!m_usable) {
            return;
        }
        // Rate and pitch come straight from the core persona, so the pacing the
        // timeline was planned with is the pacing that gets spoken.
        //
        // Warmth is not read here. It used to drive volume, which was only
        // defensible while the dial never moved; now that it drifts with the
        // mood, the same mapping would make the assistant audibly pump quieter
        // and louder as it got thoughtful. Volume is left alone and warmth is
        // left to an engine that can express it as pace.
        m_voice.setRate(std::clamp(static_cast<double>(persona.rate) - 1.0, -1.0, 1.0));
        m_voice.setPitch(std::clamp(static_cast<double>(persona.pitch) * kPitchPerSemitone, -1.0, 1.0));
    }

    /// Hands an utterance to Qt's platform text-to-speech engine.
    void speak(const QString& text) override { m_voice.say(text); }

    // finishCurrent() is the clause-boundary stop: Qt's Utterance hint lets the
    // utterance in flight finish before the engine goes quiet, which is exactly
    // what a barge-in should do and is far more accurate than a timer guessing
    // how long a word takes.
    void stop() override { m_voice.stop(QTextToSpeech::BoundaryHint::Utterance); }

    /// Requests an immediate stop from the platform text-to-speech engine.
    void stopImmediately() override { m_voice.stop(QTextToSpeech::BoundaryHint::Immediate); }

    /// Returns whether the platform voice is speaking or synthesizing audio.
    bool speakingNow() const override {
        return m_voice.state() == QTextToSpeech::Speaking
            || m_voice.state() == QTextToSpeech::Synthesizing;
    }

    // Called by the synthesizer's stateChanged hook.
    void observeState() {
        switch (m_voice.state()) {
        case QTextToSpeech::Speaking:
        case QTextToSpeech::Synthesizing:
        case QTextToSpeech::Paused:
            return; // still in flight
        case QTextToSpeech::Error:
            reportFailed(m_voice.errorString().isEmpty()
                             ? QStringLiteral("The speech engine reported an error. Continuing as text.")
                             : m_voice.errorString());
            return;
        case QTextToSpeech::Ready:
            break;
        }
        reportFinished();
    }

    /// Returns the Qt voice engine for initial selection and signal wiring.
    QTextToSpeech& voice() { return m_voice; }

private:
    QTextToSpeech m_voice;
    bool m_usable = false;
    QString m_description;
};
#else
// A build without the module still constructs a backend rather than branching
// everywhere at the call site, and it says plainly why there is no voice.
class PlatformSpeechBackend final : public SpeechBackend {
public:
    /// Reports that platform speech is unavailable in builds without Qt TextToSpeech.
    bool usable() const override { return false; }
    /// Explains why this build has no platform speech output.
    QString description() const override {
        return QStringLiteral("Qt text-to-speech not compiled in");
    }
    /// Ignores voice settings because this build has no speech engine.
    void applyVoice(const core::VoicePersona&) override {}
    /// Ignores speech requests because this build has no speech engine.
    void speak(const QString&) override {}
    /// Does nothing because this build cannot have an active utterance.
    void stop() override {}
    /// Does nothing because this build cannot have audio to stop immediately.
    void stopImmediately() override {}
    /// Returns false because this build cannot speak through the platform backend.
    bool speakingNow() const override { return false; }

private:
    QString m_description;
};
#endif

// Holds nothing beyond the backend choice; it exists so the header can name an
// incomplete type without every user of it including a platform header.
class SpeechSynthesizerPrivate {};

/// Configures pause/load timers and selects the best available platform voice.
SpeechSynthesizer::SpeechSynthesizer(QObject* parent)
    : QObject(parent), d(std::make_unique<SpeechSynthesizerPrivate>()) {
    m_pauseTimer.setSingleShot(true);
    connect(&m_pauseTimer, &QTimer::timeout, this, &SpeechSynthesizer::speakNow);
    m_voiceLoadDeadline.setSingleShot(true);
    connect(&m_voiceLoadDeadline, &QTimer::timeout, this, &SpeechSynthesizer::onVoiceLoadDeadline);
    m_voiceLoadTimeoutMs = kVoiceLoadDeadlineMs;

    m_backend = std::make_unique<PlatformSpeechBackend>();
    m_backend->setCallbacks([this] { onBackendFinished(); },
                            [this](const QString& reason) { onBackendFailed(reason); });

#if KESTREL_HAS_TEXT_TO_SPEECH
    // A voice that is installed but not usable is worse than none: speak()
    // would fail on the first clause and the reply would stop halfway. So the
    // check happens once, here, and the answer does not change at runtime.
    auto* platform = static_cast<PlatformSpeechBackend*>(m_backend.get());
    const QList<QVoice> voices = platform->voice().availableVoices();
    if (voices.isEmpty()) {
        // The module is compiled in but no engine answered. The backend stays
        // in place and simply stays quiet, and it says why, so the panel shows
        // a reason instead of an empty voice line.
        platform->setAvailability(
            false, QStringLiteral("no speech engine is available on this machine"));
    } else {
        // The system's preferred language decides the match, not the user's
        // locale: a voice has to be able to say the words.
        const QLocale systemLocale = QLocale::system();
        const QVoice chosen = chooseVoice(voices, systemLocale);
        platform->voice().setVoice(chosen);
        platform->setAvailability(true, chosen.name());
        connect(&platform->voice(), &QTextToSpeech::stateChanged, this, [this] {
            static_cast<PlatformSpeechBackend*>(m_backend.get())->observeState();
        });
    }
#endif
}

/// Releases the owned speech backend and private implementation state.
SpeechSynthesizer::~SpeechSynthesizer() = default;

/// Transfers ownership of a speech backend and wires its callbacks.
void SpeechSynthesizer::adoptBackend(std::unique_ptr<SpeechBackend> backend) {
    setBackendForTesting(std::move(backend));
}

/// Prefetches a future clause, deferring it until any paused clause has reached the engine.
void SpeechSynthesizer::prefetch(const QString& text) {
    if (m_backend == nullptr || text.trimmed().isEmpty()) {
        return;
    }
    // A clause with a leading pause has not reached the engine yet: it is being
    // held back so the gap before it is silence in the right place rather than
    // silence somewhere else. Handing the following clause over first would ask
    // the engine for the second clause before the first, and the engine numbers
    // the audio it produces in the order it was asked -- so the reply's first
    // sentence would be written to clause-1 and its second to clause-0. The
    // lookahead therefore waits for the clause in front of it and leaves in the
    // same instant, which costs none of the overlap it exists for: synthesis of
    // the next clause still runs while the current one is being heard.
    if (!m_pending.trimmed().isEmpty()) {
        m_queuedPrefetch = text;
        return;
    }
    m_backend->prefetch(text);
}

/// Returns the active backend's voice choices, or an empty list without a backend.
QStringList SpeechSynthesizer::voiceChoices() const {
    return m_backend != nullptr ? m_backend->voiceChoices() : QStringList();
}

/// Returns the selected backend voice, or an empty string without a backend.
QString SpeechSynthesizer::currentVoice() const {
    return m_backend != nullptr ? m_backend->currentVoice() : QString();
}

/// Asks the backend to select a voice and returns whether it accepted the choice.
bool SpeechSynthesizer::setVoice(const QString& voice) {
    return m_backend != nullptr && m_backend->setVoice(voice);
}

/// Replaces a nonnull backend, clearing queued clauses and installing callbacks.
void SpeechSynthesizer::setBackendForTesting(std::unique_ptr<SpeechBackend> backend) {
    if (backend == nullptr) {
        return;
    }
    m_pauseTimer.stop();
    m_pending.clear();
    m_pendingText.clear();
    m_queuedPrefetch.clear();
    m_stoppingAtBoundary = false;
    m_backend = std::move(backend);
    m_backend->setCallbacks([this] { onBackendFinished(); },
                            [this](const QString& reason) { onBackendFailed(reason); });
    m_backend->setAvailabilityCallbacks(
        [this] { onBackendAvailable(); },
        [this] { onBackendUnavailable(); });
}

/// Clears the loading timeout state and announces that speech is available.
void SpeechSynthesizer::onBackendAvailable() {
    // A voice that turns up is a voice worth waiting for again, and the reason
    // it was given up on no longer describes anything.
    m_voiceGaveUp = false;
    m_voiceLoadDeadline.stop();
    emit availabilityChanged(true);
}

/// Releases any audio hold and announces that the backend is unavailable.
void SpeechSynthesizer::onBackendUnavailable() {
    // The engine will never answer, so waiting longer cannot produce audio.
    releaseOwedAudio();
    emit availabilityChanged(false);
}

/// Marks voice loading as timed out and releases any response waiting for audio.
void SpeechSynthesizer::onVoiceLoadDeadline() {
    // The engine is not going to say it is ready. Half-synced weights, a wedged
    // interpreter and a load that ran out of memory are indistinguishable from
    // here, and none of them is fixed by waiting longer.
    m_voiceGaveUp = true;
    releaseOwedAudio();
}

/// Returns whether a response is being held for a loading voice.
bool SpeechSynthesizer::audioOwed() const {
    return m_audioOwed;
}

/// Holds a response for a present but unready voice and starts its loading deadline.
void SpeechSynthesizer::oweAudio() {
    m_voiceLoadDeadline.stop();
    // Holding every reply for the same voice that already failed would give the
    // user the same silence over and over instead of an answer.
    m_audioOwed = !available() && present() && !m_voiceGaveUp;
    if (m_audioOwed) {
        m_voiceLoadDeadline.start(m_voiceLoadTimeoutMs);
    }
}

/// Clears an audio hold and emits its release once, including whether loading timed out.
void SpeechSynthesizer::releaseOwedAudio() {
    m_voiceLoadDeadline.stop();
    if (!m_audioOwed) {
        return;
    }
    // Cleared before the signal so a handler that reaches back here cannot find
    // the hold still open and release it twice.
    m_audioOwed = false;
    emit owedAudioReleased(m_voiceGaveUp);
}

/// Sets the deadline duration in milliseconds for future voice loading holds.
void SpeechSynthesizer::setVoiceLoadTimeout(int ms) {
    m_voiceLoadTimeoutMs = ms;
}

/// Returns whether the current backend can produce audio.
bool SpeechSynthesizer::available() const {
    return m_backend != nullptr && m_backend->usable();
}

/// Returns whether a backend is present, including an installed voice still loading.
bool SpeechSynthesizer::present() const {
    return m_backend != nullptr && m_backend->present();
}

/// Returns the backend's voice description, or an unavailable label.
QString SpeechSynthesizer::voiceDescription() const {
    return m_backend != nullptr ? m_backend->description() : QStringLiteral("unavailable");
}

/// Applies a voice persona only when the current backend is usable.
void SpeechSynthesizer::applyVoice(const core::VoicePersona& persona) {
    if (!available()) {
        return;
    }
    m_backend->applyVoice(persona);
}

/// Schedules a clause after a pause capped at 1200 ms, or skips unavailable/empty speech.
void SpeechSynthesizer::speak(const QString& text, int leadingPauseMs) {
    if (!available() || text.trimmed().isEmpty()) {
        // Nothing to say is not a failure. An empty clause is skipped and the
        // controller keeps pulling segments until the response is drained.
        QMetaObject::invokeMethod(this, [this] { emit segmentFinished(); }, Qt::QueuedConnection);
        return;
    }
    m_pending = text;
    m_stoppingAtBoundary = false;
    if (leadingPauseMs > 0) {
        m_pauseTimer.start(std::min(leadingPauseMs, kMaxPauseMs));
        return;
    }
    speakNow();
}

/// Cancels a pending pause or asks the engine to stop at the current utterance boundary.
void SpeechSynthesizer::requestStop() {
    if (!available() || (!speaking() && !waiting())) {
        emit stopCompleted();
        return;
    }
    if (waiting()) {
        // Still counting down into a clause, so there is nothing in flight to
        // let finish: this is already a boundary.
        m_pauseTimer.stop();
        m_pending.clear();
        m_queuedPrefetch.clear();
        emit stopCompleted();
        return;
    }
    // A clause is in flight. Let it land -- cutting mid-word is the thing this
    // whole segment-at-a-time design exists to avoid. The engine is told to stop
    // at the next utterance boundary, and the flag is what makes the resulting
    // Ready state report a stop rather than a finished segment.
    m_stoppingAtBoundary = true;
    m_backend->stop();
}

/// Clears queued speech, requests an immediate backend stop, and reports stop completion.
void SpeechSynthesizer::stopNow() {
    m_pauseTimer.stop();
    m_pending.clear();
    m_pendingText.clear();
    m_queuedPrefetch.clear();
    m_stoppingAtBoundary = false;
    if (available()) {
        m_backend->stopImmediately();
    }
    emit stopCompleted();
}

/// Returns whether the backend reports speech in progress.
bool SpeechSynthesizer::speaking() const noexcept {
    return m_backend != nullptr && m_backend->speakingNow();
}

/// Returns whether a leading pause is delaying the next clause.
bool SpeechSynthesizer::waiting() const noexcept {
    return m_pauseTimer.isActive();
}

/// Submits the pending clause, then its queued lookahead, in that order.
void SpeechSynthesizer::speakNow() {
    if (!available() || m_pending.trimmed().isEmpty()) {
        emit segmentFinished();
        return;
    }
    m_pendingText = m_pending;
    m_pending.clear();
    m_backend->speak(m_pendingText);
    // The clause in front is now the engine's problem, so the lookahead goes
    // with it. At most one can ever be waiting: the pump asks for a lookahead
    // once per clause, and each clause is committed before the next is taken.
    if (!m_queuedPrefetch.isEmpty()) {
        const QString ahead = m_queuedPrefetch;
        m_queuedPrefetch.clear();
        m_backend->prefetch(ahead);
    }
}

/// Reports either clause completion or completion of a requested boundary stop.
void SpeechSynthesizer::onBackendFinished() {
    m_pendingText.clear();
    if (m_stoppingAtBoundary) {
        m_stoppingAtBoundary = false;
        emit stopCompleted();
        return;
    }
    // Ready after speaking means the clause finished. The controller decides
    // whether that means another clause or the end of the response.
    emit segmentFinished();
}

/// Clears queued speech and reports the backend failure followed by stop completion.
void SpeechSynthesizer::onBackendFailed(const QString& reason) {
    m_pauseTimer.stop();
    m_pending.clear();
    m_pendingText.clear();
    m_queuedPrefetch.clear();
    m_stoppingAtBoundary = false;
    emit failed(reason);
    emit stopCompleted();
}

} // namespace kestrel::app
