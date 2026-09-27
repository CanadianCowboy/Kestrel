#include "app/localmodelspeechbackend.h"

#include <QAudioDevice>
#include <QAudioOutput>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QTemporaryDir>
#include <QUrl>

namespace kestrel::app {

LocalModelSpeechBackend::LocalModelSpeechBackend(QObject* parent)
    : QObject(parent) {
    m_scratch = std::make_unique<QTemporaryDir>();
    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            &LocalModelSpeechBackend::onMediaStatusChanged);
}

void LocalModelSpeechBackend::completeSetup() {
    if (!m_scratch->isValid()) {
        return;
    }
    // The engine's own order is the order of preference: Kokoro leads with the
    // British male voices because that is the register the voice was chosen for.
    m_voice = engineVoices().value(0);
    startEngine();
}

LocalModelSpeechBackend::~LocalModelSpeechBackend() {
    // Release the audio before the scratch directory goes. A player still
    // holding the last clause keeps a handle on it, and a temporary directory
    // that cannot delete itself leaves a file behind on every run.
    if (m_player != nullptr) {
        m_player->stop();
        m_player->setSource(QUrl());
    }
    if (m_audio != nullptr) {
        m_audio->setDevice(QAudioDevice());
    }
}

bool LocalModelSpeechBackend::usable() const {
    // Usable as soon as the engine is up, not as soon as a model has finished
    // loading. An engine that is still warming up can still be handed work, and
    // making the first reply wait for a start-up signal would put a visible
    // pause where none is needed.
    return m_started;
}

QString LocalModelSpeechBackend::description() const {
    if (!m_started) {
        return tr("no %1 voice is installed").arg(engineName());
    }
    return QStringLiteral("%1 (%2, local)").arg(m_voice, engineName());
}

void LocalModelSpeechBackend::failIfNotStarted() {
    m_started = false;
}

QString LocalModelSpeechBackend::engineExecutable() const {
    return {};
}

void LocalModelSpeechBackend::applyVoice(const core::VoicePersona& persona) {
    // The persona's rate is a speaking rate, not a playback rate, so it is
    // handed to the model rather than applied to finished audio. Pitch and
    // warmth have no equivalent here and are ignored rather than approximated: a
    // voice shifted by a resampler is a voice that sounds broken, and saying so
    // is better than doing it.
    m_speed = 0.5 + static_cast<double>(persona.rate);
}

QString LocalModelSpeechBackend::nextScratchPath() {
    return m_scratch->filePath(QStringLiteral("clause-%1.wav").arg(m_clause++));
}

void LocalModelSpeechBackend::speak(const QString& text) {
    if (!m_started || m_speaking || text.trimmed().isEmpty()) {
        return;
    }
    m_speaking = true;

    // Already made: the common case, because the caller warns the engine about a
    // clause while the previous one is still being spoken.
    const QString ready = takePrefetched(text);
    if (!ready.isEmpty() && QFileInfo::exists(ready)) {
        m_player->setSource(QUrl::fromLocalFile(ready));
        m_player->play();
        return;
    }

    m_pending.append(PendingRequest{text, true});
    synthesise(text, nextScratchPath());
}

void LocalModelSpeechBackend::prefetch(const QString& text) {
    if (!m_started || text.trimmed().isEmpty() || m_prefetched.contains(text)) {
        return;
    }
    for (const PendingRequest& request : m_pending) {
        if (request.text == text) {
            return;
        }
    }
    m_pending.append(PendingRequest{text, false});
    synthesise(text, nextScratchPath());
}

QString LocalModelSpeechBackend::takePrefetched(const QString& text) {
    const auto match = m_prefetched.find(text);
    if (match == m_prefetched.end()) {
        return {};
    }
    const QString path = match.value();
    m_prefetched.erase(match);
    return path;
}

void LocalModelSpeechBackend::noteSynthesised(const QString& path) {
    if (m_pending.isEmpty()) {
        // Nothing asked for this. Playing it anyway would speak a clause the
        // user has moved past, so it is dropped.
        return;
    }
    // Engines answer in the order they were written to, so the front of the
    // queue is what this reply belongs to.
    const PendingRequest request = m_pending.takeFirst();
    if (request.playsNow) {
        m_player->setSource(QUrl::fromLocalFile(path));
        m_player->play();
    } else {
        // Asked for early, so it waits rather than cutting off the clause the
        // user is hearing right now.
        m_prefetched.insert(request.text, path);
    }
}

void LocalModelSpeechBackend::noteClauseFailed(const QString& reason) {
    if (m_pending.isEmpty()) {
        return;
    }
    m_pending.takeFirst();
    m_speaking = false;
    reportFailed(reason);
}

void LocalModelSpeechBackend::noteEngineFailed(const QString& reason) {
    m_started = false;
    m_speaking = false;
    m_pending.clear();
    reportFailed(reason.isEmpty() ? tr("the local voice failed") : reason);
}

void LocalModelSpeechBackend::stop() {
    // A clause already handed to the model is not recalled; it finishes, which is
    // the same bargain the platform voice makes and the reason a barge-in lands
    // between clauses rather than mid-word.
    m_player->stop();
}

void LocalModelSpeechBackend::stopImmediately() {
    m_player->stop();
    m_speaking = false;
}

bool LocalModelSpeechBackend::speakingNow() const {
    return m_speaking;
}

bool LocalModelSpeechBackend::setVoice(const QString& voice) {
    if (!engineVoices().contains(voice)) {
        // Refused rather than accepted and hoped for: a name the model does not
        // have would fail at synthesis time, in the middle of a reply, which is
        // the worst moment to discover a typo.
        return false;
    }
    if (voice == m_voice) {
        return true;
    }
    m_voice = voice;
    // Anything already made is in the old voice. Mixing them in one reply would
    // sound like two people, so the cache is dropped and the next clause is
    // built afresh.
    m_prefetched.clear();
    return true;
}

void LocalModelSpeechBackend::onMediaStatusChanged() {
    if (m_player->mediaStatus() != QMediaPlayer::EndOfMedia) {
        return;
    }
    m_speaking = false;
    reportFinished();
}

} // namespace kestrel::app
