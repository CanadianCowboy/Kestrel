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

void LocalModelSpeechBackend::markGivenUp() {
    // Whether anything was promised to the synthesizer, not whether it ever
    // came true. An engine that launched and then failed while loading its
    // model has still made a promise -- present() said a voice was coming, and
    // a reply may already be held waiting for it -- so it has to be withdrawn
    // here. Reporting only for a started engine left that case silent, and the
    // reply then waited out the whole 15 second load deadline for an engine
    // already known to be dead.
    const bool wasPromised = m_launched || m_started;
    m_launched = false;
    m_started = false;
    if (wasPromised) {
        reportUnavailable();
    }
}

QString LocalModelSpeechBackend::engineExecutable() const {
    return {};
}

void LocalModelSpeechBackend::applyVoice(const core::VoicePersona& persona) {
    // The persona's rate and warmth are speaking decisions, not playback ones, so
    // they are handed to the model rather than applied to finished audio. Pitch
    // has no equivalent here and is ignored rather than approximated: a voice
    // shifted by a resampler is a voice that sounds broken, and saying so is
    // better than doing it.
    m_speed = core::paceFor(persona);
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

    for (PendingRequest& request : m_pending) {
        if (request.text == text) {
            request.playsNow = true;
            return;
        }
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
    m_speaking = false;
    m_pending.clear();
    markGivenUp();
    reportFailed(reason.isEmpty() ? tr("the local voice failed") : reason);
}

void LocalModelSpeechBackend::stop() {
    // A clause already handed to the player is not recalled; it finishes, which
    // is the same bargain the platform voice makes with BoundaryHint::Utterance
    // and the reason a barge-in lands between clauses rather than mid-word.
    //
    // Calling m_player->stop() here instead is the obvious thing to write and
    // it is wrong twice over. It cuts the audio in the middle of a word, and it
    // moves the media status to LoadedMedia rather than EndOfMedia -- so the
    // finish never arrives, m_speaking stays true, and the synthesizer sits
    // waiting for the stopCompleted() that only onBackendFinished() can send.
    // Every later clause is then refused at the top of speak() and the voice
    // goes silent for the rest of the session.
    //
    // So nothing is cut here. What is dropped is everything that was asked for
    // and is not yet playing: a stop is a decision about what is heard next,
    // and work queued behind it should not start after it.
    for (PendingRequest& request : m_pending) {
        request.playsNow = false;
    }
    m_prefetched.clear();
    if (m_player->playbackState() == QMediaPlayer::PlayingState) {
        // Something is genuinely being heard, so this is a real boundary and
        // the finish will arrive on its own through EndOfMedia.
        return;
    }
    // Nothing is playing. Either the clause is still being synthesised or there
    // is nothing in flight at all, and both are already a boundary. The finish
    // is reported here because nobody else is going to report it: no audio
    // means no EndOfMedia, and the synthesizer is waiting.
    m_speaking = false;
    reportFinished();
}

void LocalModelSpeechBackend::stopImmediately() {
    m_player->stop();
    // A clause still being synthesised must not begin playing afterwards.
    // noteSynthesised() takes the front of the queue whatever its flag says, so
    // without this a cancel landing during synthesis is followed by the very
    // clause it cancelled, and that clause's EndOfMedia reports a segment
    // finishing that nobody asked for.
    for (PendingRequest& request : m_pending) {
        request.playsNow = false;
    }
    // The prefetch cache survives: it is keyed by text, and a later reply that
    // happens to contain this clause is a legitimate hit.
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
