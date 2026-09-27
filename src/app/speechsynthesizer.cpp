#include "app/speechsynthesizer.h"

#include <QMetaObject>

#include <algorithm>

#if KESTREL_HAS_TEXT_TO_SPEECH
#include <QTextToSpeech>
#include <QVoice>
#endif

namespace kestrel::app {

namespace {

// Longest gap between clauses. A pause longer than this is not pacing, it is a
// hang, and clamping keeps one odd plan from stalling the whole response.
constexpr int kMaxPauseMs = 1200;

// QTextToSpeech expresses pitch on a -50..50 scale while the core voice
// persona counts semitones. Four per semitone puts the useful range of about
// an octave at either end of the scale, and stops short of the point where it
// stops sounding like a person and starts sounding broken.
constexpr float kPitchPerSemitone = 4.0F;

} // namespace

#if KESTREL_HAS_TEXT_TO_SPEECH
// The platform voice. This translation unit is the only place that knows the
// module exists, which is what lets the app build and behave identically
// without it.
class PlatformSpeechBackend final : public SpeechBackend {
public:
    bool usable() const override { return m_usable; }
    QString description() const override { return m_description; }

    // The synthesizer decides once whether an engine answered and records the
    // answer here. Without a setter the flags below would keep their defaults
    // forever and a perfectly good voice would read as absent.
    void setAvailability(bool usable, const QString& description) {
        m_usable = usable;
        m_description = description;
    }

    void applyVoice(const core::VoicePersona& persona) override {
        if (!m_usable) {
            return;
        }
        // Rate and pitch come straight from the core persona, so the pacing the
        // timeline was planned with is the pacing that gets spoken. Volume
        // carries the warmth dial, which has no other meaning at playback time.
        m_voice.setRate(static_cast<double>(persona.rate));
        m_voice.setPitch(static_cast<double>(persona.pitch) * kPitchPerSemitone);
        m_voice.setVolume(0.5 + 0.5 * static_cast<double>(persona.warmth));
    }

    void speak(const QString& text) override { m_voice.say(text); }

    // finishCurrent() is the clause-boundary stop: Qt's Utterance hint lets the
    // utterance in flight finish before the engine goes quiet, which is exactly
    // what a barge-in should do and is far more accurate than a timer guessing
    // how long a word takes.
    void stop() override { m_voice.stop(QTextToSpeech::BoundaryHint::Utterance); }

    void stopImmediately() override { m_voice.stop(QTextToSpeech::BoundaryHint::Immediate); }

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
    bool usable() const override { return false; }
    QString description() const override {
        return QStringLiteral("Qt text-to-speech not compiled in");
    }
    void applyVoice(const core::VoicePersona&) override {}
    void speak(const QString&) override {}
    void stop() override {}
    void stopImmediately() override {}
    bool speakingNow() const override { return false; }

private:
    QString m_description;
};
#endif

// Holds nothing beyond the backend choice; it exists so the header can name an
// incomplete type without every user of it including a platform header.
class SpeechSynthesizerPrivate {};

SpeechSynthesizer::SpeechSynthesizer(QObject* parent)
    : QObject(parent), d(std::make_unique<SpeechSynthesizerPrivate>()) {
    m_pauseTimer.setSingleShot(true);
    connect(&m_pauseTimer, &QTimer::timeout, this, &SpeechSynthesizer::speakNow);

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
        platform->voice().setVoice(voices.first());
        platform->setAvailability(true, voices.first().name());
        connect(&platform->voice(), &QTextToSpeech::stateChanged, this, [this] {
            static_cast<PlatformSpeechBackend*>(m_backend.get())->observeState();
        });
    }
#endif
}

SpeechSynthesizer::~SpeechSynthesizer() = default;

void SpeechSynthesizer::setBackendForTesting(std::unique_ptr<SpeechBackend> backend) {
    if (backend == nullptr) {
        return;
    }
    m_pauseTimer.stop();
    m_pending.clear();
    m_pendingText.clear();
    m_stoppingAtBoundary = false;
    m_backend = std::move(backend);
    m_backend->setCallbacks([this] { onBackendFinished(); },
                            [this](const QString& reason) { onBackendFailed(reason); });
}

bool SpeechSynthesizer::available() const {
    return m_backend != nullptr && m_backend->usable();
}

QString SpeechSynthesizer::voiceDescription() const {
    return m_backend != nullptr ? m_backend->description() : QStringLiteral("unavailable");
}

void SpeechSynthesizer::applyVoice(const core::VoicePersona& persona) {
    if (!available()) {
        return;
    }
    m_backend->applyVoice(persona);
}

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

void SpeechSynthesizer::stopNow() {
    m_pauseTimer.stop();
    m_pending.clear();
    m_pendingText.clear();
    m_stoppingAtBoundary = false;
    if (available()) {
        m_backend->stopImmediately();
    }
    emit stopCompleted();
}

bool SpeechSynthesizer::speaking() const noexcept {
    return m_backend != nullptr && m_backend->speakingNow();
}

bool SpeechSynthesizer::waiting() const noexcept {
    return m_pauseTimer.isActive();
}

void SpeechSynthesizer::speakNow() {
    if (!available() || m_pending.trimmed().isEmpty()) {
        emit segmentFinished();
        return;
    }
    m_pendingText = m_pending;
    m_pending.clear();
    m_backend->speak(m_pendingText);
}

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

void SpeechSynthesizer::onBackendFailed(const QString& reason) {
    m_pauseTimer.stop();
    m_pending.clear();
    m_pendingText.clear();
    m_stoppingAtBoundary = false;
    emit failed(reason);
    emit stopCompleted();
}

} // namespace kestrel::app
