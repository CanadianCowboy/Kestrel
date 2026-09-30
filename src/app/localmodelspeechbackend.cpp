#include "app/localmodelspeechbackend.h"

#include <QAudioDevice>
#include <QAudioOutput>
#include <QFileInfo>
#include <QMediaPlayer>
#include <QTemporaryDir>
#include <QUrl>

namespace kestrel::app {

/// Creates temporary audio storage and connects the media player's completion signal.
LocalModelSpeechBackend::LocalModelSpeechBackend(QObject* parent)
    : QObject(parent) {
    m_scratch = std::make_unique<QTemporaryDir>();
    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            &LocalModelSpeechBackend::onMediaStatusChanged);
}

/// Chooses the first engine voice and starts the engine if temporary storage is valid.
void LocalModelSpeechBackend::completeSetup() {
    if (!m_scratch->isValid()) {
        // No scratch directory means no audio files, so the engine can never
        // answer. Reported, so a response waiting on a voice is not held for
        // one that cannot exist.
        markGivenUp();
        return;
    }
    // The engine's own order is the order of preference: Kokoro leads with the
    // British male voices because that is the register the voice was chosen for.
    m_voice = engineVoices().value(0);
    startEngine();
}

/// Releases the media file and audio device before temporary audio storage is destroyed.
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

/// Returns whether the engine has announced readiness to produce audio.
bool LocalModelSpeechBackend::usable() const {
    // Usable as soon as the engine is up, not as soon as a model has finished
    // loading. An engine that is still warming up can still be handed work, and
    // making the first reply wait for a start-up signal would put a visible
    // pause where none is needed.
    return m_started;
}

/// Describes the current voice, loading state, or missing local engine.
QString LocalModelSpeechBackend::description() const {
    if (m_started) {
        return QStringLiteral("%1 (%2, local)").arg(m_voice, engineName());
    }
    // Two different situations that both mean "no audio yet", and telling them
    // apart is the difference between a voice that is starting and a machine
    // that does not have one.
    if (m_launched) {
        return tr("the %1 voice is still loading").arg(engineName());
    }
    return tr("no %1 voice is installed").arg(engineName());
}

/// Clears launch/readiness state and reports unavailability if the engine was ready.
void LocalModelSpeechBackend::markGivenUp() {
    m_launched = false;
    if (!m_started) {
        return;
    }
    m_started = false;
    reportUnavailable();
}

/// Returns an empty executable path unless a concrete engine overrides it.
QString LocalModelSpeechBackend::engineExecutable() const {
    return {};
}

/// Updates synthesis speed from persona rate and warmth; pitch is not applied.
void LocalModelSpeechBackend::applyVoice(const core::VoicePersona& persona) {
    // The persona's rate and warmth are speaking decisions, not playback ones, so
    // they are handed to the model rather than applied to finished audio. Pitch
    // has no equivalent here and is ignored rather than approximated: a voice
    // shifted by a resampler is a voice that sounds broken, and saying so is
    // better than doing it.
    m_speed = core::paceFor(persona);
}

/// Allocates the next numbered WAV path in the temporary directory.
QString LocalModelSpeechBackend::nextScratchPath() {
    return m_scratch->filePath(QStringLiteral("clause-%1.wav").arg(m_clause++));
}

/// Plays cached audio or requests synthesis for a nonempty clause when ready and idle.
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

    for (auto& request : m_pending) {
        if (!request.obsolete && request.text == text) {
            request.playsNow = true;
            return;
        }
    }
    m_pending.append(PendingRequest{text, true});
    synthesise(text, nextScratchPath());
}

/// Requests audio for a future clause unless it is already cached or pending.
void LocalModelSpeechBackend::prefetch(const QString& text) {
    if (!m_started || text.trimmed().isEmpty() || m_prefetched.contains(text)) {
        return;
    }
    for (const PendingRequest& request : m_pending) {
        if (!request.obsolete && request.text == text) {
            return;
        }
    }
    m_pending.append(PendingRequest{text, false});
    synthesise(text, nextScratchPath());
}

/// Removes and returns cached audio for the text, or an empty path when absent.
QString LocalModelSpeechBackend::takePrefetched(const QString& text) {
    const auto match = m_prefetched.find(text);
    if (match == m_prefetched.end()) {
        return {};
    }
    const QString path = match.value();
    m_prefetched.erase(match);
    return path;
}

/// Matches an audio file to the oldest pending request and plays or caches it.
void LocalModelSpeechBackend::noteSynthesised(const QString& path) {
    if (m_pending.isEmpty()) {
        // Nothing asked for this. Playing it anyway would speak a clause the
        // user has moved past, so it is dropped.
        return;
    }
    // Engines answer in the order they were written to, so the front of the
    // queue is what this reply belongs to.
    const PendingRequest request = m_pending.takeFirst();
    if (request.obsolete) {
        return;
    }
    if (request.playsNow) {
        m_player->setSource(QUrl::fromLocalFile(path));
        m_player->play();
    } else {
        // Asked for early, so it waits rather than cutting off the clause the
        // user is hearing right now.
        m_prefetched.insert(request.text, path);
    }
}

/// Drops the oldest pending clause and reports its synthesis failure.
void LocalModelSpeechBackend::noteClauseFailed(const QString& reason) {
    if (m_pending.isEmpty()) {
        return;
    }
    const auto request = m_pending.takeFirst();
    if (request.obsolete || !request.playsNow) {
        return;
    }
    m_speaking = false;
    reportFailed(reason);
}

/// Clears pending synthesis, marks the engine unavailable, and reports the failure.
void LocalModelSpeechBackend::noteEngineFailed(const QString& reason) {
    m_speaking = false;
    m_pending.clear();
    markGivenUp();
    reportFailed(reason.isEmpty() ? tr("the local voice failed") : reason);
}

/// Lets playing audio reach its boundary and prevents pending audio from playing.
void LocalModelSpeechBackend::stop() {
    for (auto& request : m_pending) {
        request.playsNow = false;
    }
    if (m_player->playbackState() != QMediaPlayer::PlayingState) {
        m_speaking = false;
        reportFinished();
    }
}

/// Stops the media player immediately and clears the speaking flag.
void LocalModelSpeechBackend::stopImmediately() {
    for (auto& request : m_pending) {
        request.playsNow = false;
    }
    m_player->stop();
    m_speaking = false;
}

/// Returns whether a clause is being synthesized for playback or spoken.
bool LocalModelSpeechBackend::speakingNow() const {
    return m_speaking;
}

/// Selects a supported voice and clears old prefetched audio; rejects unknown voices.
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
    QList<PendingRequest> replacements;
    for (auto& request : m_pending) {
        if (!request.obsolete) {
            replacements.append(request);
            request.obsolete = true;
        }
    }
    // Keep old queue slots until their replies arrive, so those replies cannot
    // be mistaken for requests synthesized with the new voice.
    for (const auto& request : replacements) {
        m_pending.append(request);
        synthesise(request.text, nextScratchPath());
    }
    return true;
}

/// Clears the speaking flag and reports completion when the player reaches end of media.
void LocalModelSpeechBackend::onMediaStatusChanged() {
    if (m_player->mediaStatus() != QMediaPlayer::EndOfMedia) {
        return;
    }
    m_speaking = false;
    reportFinished();
}

} // namespace kestrel::app
