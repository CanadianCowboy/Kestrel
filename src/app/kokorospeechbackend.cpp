#include "app/kokorospeechbackend.h"

#include <QAudioDevice>
#include <QAudioOutput>
#include <QDir>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaPlayer>
#include <QProcess>
#include <QTemporaryDir>

namespace kestrel::app {

namespace {

// One clause in flight at a time, and the synthesizer is what serialises them.
// This only has to be distinctive enough that two runs cannot collide.
QString clauseName(int index) {
    return QStringLiteral("clause-%1.wav").arg(index);
}

} // namespace

KokoroSpeechBackend::KokoroSpeechBackend(QString python, QString serverScript, QObject* parent)
    : QObject(parent),
      m_python(std::move(python)),
      m_serverScript(std::move(serverScript)),
      m_voice(QStringLiteral("bm_george")) {
    m_scratch = std::make_unique<QTemporaryDir>();
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    m_player = new QMediaPlayer(this);
    m_audio = new QAudioOutput(this);
    m_player->setAudioOutput(m_audio);
    connect(m_player, &QMediaPlayer::mediaStatusChanged, this,
            &KokoroSpeechBackend::onMediaStatusChanged);

    connect(m_process, &QProcess::readyReadStandardOutput, this,
            &KokoroSpeechBackend::onReadyRead);
    connect(m_process, &QProcess::errorOccurred, this, &KokoroSpeechBackend::onProcessError);
    connect(m_process, &QProcess::finished, this, &KokoroSpeechBackend::onProcessFinished);

    if (!m_scratch->isValid() || m_python.isEmpty() || m_serverScript.isEmpty()) {
        // Reported, not thrown. The synthesizer falls back to the platform
        // voice, and the panel says which one it got.
        return;
    }

    m_process->start(m_python, {m_serverScript});
    // The model takes a moment to load. Anything longer than this is a machine
    // that cannot do it, and waiting longer would only delay the first reply.
    if (!m_process->waitForStarted(5000)) {
        return;
    }
    m_started = true;
}

KokoroSpeechBackend::~KokoroSpeechBackend() {
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
    if (m_process != nullptr && m_process->state() != QProcess::NotRunning) {
        m_process->closeWriteChannel();
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

bool KokoroSpeechBackend::usable() const {
    // Usable as soon as the process is up, not as soon as the model has loaded.
    // The server reads requests only after it is ready, so a clause sent during
    // the load is queued and answered a moment later rather than lost -- which
    // is what lets the first reply start speaking without a visible pause for
    // start-up.
    return m_started;
}

QString KokoroSpeechBackend::description() const {
    if (!m_started) {
        return tr("no local neural voice is installed");
    }
    if (!m_ready) {
        // Distinct from "not installed" on purpose: the voice exists and is
        // loading, and a user told the wrong one of those would go looking for
        // something to install.
        return tr("%1 (Kokoro, loading)").arg(m_voice);
    }
    return tr("%1 (Kokoro, local)").arg(m_voice);
}

// The English voices in the shipped model. Listed rather than discovered at
// runtime: the voice table is a file on disk, and parsing it to build a list for
// a picker would be a second source of truth to keep in step with the model's
// contents. Grouped the way the picker reads best -- by accent and gender --
// because "am_michael" means nothing to anyone who has not read the model card.
QStringList KokoroSpeechBackend::voiceChoices() const {
    static const QStringList kVoices = {
        // British male, first: this is the register the voice was chosen for.
        QStringLiteral("bm_george"),  QStringLiteral("bm_fable"),
        QStringLiteral("bm_daniel"),   QStringLiteral("bm_lewis"),
        // American male.
        QStringLiteral("am_michael"),  QStringLiteral("am_onyx"),
        QStringLiteral("am_fenrir"),   QStringLiteral("am_adam"),
        QStringLiteral("am_echo"),     QStringLiteral("am_eric"),
        QStringLiteral("am_liam"),     QStringLiteral("am_puck"),
        QStringLiteral("am_santa"),
        // British female.
        QStringLiteral("bf_emma"),     QStringLiteral("bf_isabella"),
        QStringLiteral("bf_alice"),    QStringLiteral("bf_lily"),
        // American female.
        QStringLiteral("af_heart"),    QStringLiteral("af_bella"),
        QStringLiteral("af_nicole"),   QStringLiteral("af_sarah"),
        QStringLiteral("af_jessica"),  QStringLiteral("af_nova"),
        QStringLiteral("af_sky"),      QStringLiteral("af_river"),
        QStringLiteral("af_kore"),     QStringLiteral("af_alloy"),
        QStringLiteral("af_aoede"),
    };
    return kVoices;
}

bool KokoroSpeechBackend::setVoice(const QString& voice) {
    if (!voiceChoices().contains(voice)) {
        // Refused rather than accepted and hoped for: a name the model does not
        // have would fail at synthesis time, in the middle of a reply, which is
        // the worst moment to discover a typo.
        return false;
    }
    m_voice = voice;
    return true;
}

void KokoroSpeechBackend::applyVoice(const core::VoicePersona& persona) {
    // The persona's rate is a speaking rate, not a playback rate, so it is
    // handed to the model rather than applied to the finished audio. Pitch and
    // warmth have no equivalent here and are ignored rather than approximated:
    // a voice shifted by a resampler is a voice that sounds broken, and saying
    // so is better than doing it.
    m_speed = 0.5 + static_cast<double>(persona.rate);
}

void KokoroSpeechBackend::requestSynthesis(const QString& text, const QString& path,
                                           bool playsNow) {
    m_pending.append(PendingRequest{text, playsNow});
    QJsonObject request;
    request.insert(QStringLiteral("text"), text);
    request.insert(QStringLiteral("voice"), m_voice);
    request.insert(QStringLiteral("speed"), m_speed);
    request.insert(QStringLiteral("out"), path);
    m_process->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n");
}

void KokoroSpeechBackend::prefetch(const QString& text) {
    if (!m_started || m_process == nullptr || text.trimmed().isEmpty()) {
        return;
    }
    if (m_prefetched.contains(text)) {
        return;
    }
    for (const PendingRequest& request : m_pending) {
        if (request.text == text) {
            return;
        }
    }
    requestSynthesis(text, m_scratch->filePath(clauseName(m_clause++)), false);
}

QString KokoroSpeechBackend::takePrefetched(const QString& text) {
    const auto match = m_prefetched.find(text);
    if (match == m_prefetched.end()) {
        return {};
    }
    const QString path = match.value();
    m_prefetched.erase(match);
    return path;
}

void KokoroSpeechBackend::speak(const QString& text) {
    if (!m_started || m_speaking || m_process == nullptr || text.trimmed().isEmpty()) {
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

    requestSynthesis(text, m_scratch->filePath(clauseName(m_clause++)), true);
    // The model answers asynchronously, so nothing is played yet. The
    // synthesizer treats this exactly like an engine that has not finished
    // speaking: the cursor does not move until the clause has been heard.
}

void KokoroSpeechBackend::stop() {
    // A clause already handed to the model is not recalled; it finishes, which
    // is the same bargain the platform voice makes and the reason a barge-in
    // lands between clauses rather than mid-word.
    m_player->stop();
}

void KokoroSpeechBackend::stopImmediately() {
    m_player->stop();
    m_speaking = false;
}

bool KokoroSpeechBackend::speakingNow() const {
    return m_speaking;
}

void KokoroSpeechBackend::onReadyRead() {
    while (m_process != nullptr && m_process->canReadLine()) {
        const QByteArray line = m_process->readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        QJsonParseError parseError{};
        const QJsonDocument document =
            QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            continue;
        }
        const QJsonObject reply = document.object();
        if (reply.value(QStringLiteral("ready")).toBool()) {
            m_ready = true;
            continue;
        }
        if (!reply.value(QStringLiteral("ok")).toBool()) {
            failWith(reply.value(QStringLiteral("error")).toString());
            continue;
        }
        const QString out = reply.value(QStringLiteral("out")).toString();
        if (m_pending.isEmpty()) {
            // Nothing asked for this. Playing it anyway would speak a clause
            // the user has moved past, so it is dropped.
            continue;
        }
        // The server answers in the order it was written to, so the front of
        // the queue is what this reply belongs to.
        const PendingRequest request = m_pending.takeFirst();
        if (request.playsNow) {
            m_player->setSource(QUrl::fromLocalFile(out));
            m_player->play();
        } else if (!request.text.isEmpty()) {
            // Asked for early, so it waits rather than cutting off the clause
            // the user is hearing right now.
            m_prefetched.insert(request.text, out);
        }
    }
}

void KokoroSpeechBackend::onMediaStatusChanged() {
    if (m_player->mediaStatus() != QMediaPlayer::EndOfMedia) {
        return;
    }
    m_speaking = false;
    reportFinished();
}

void KokoroSpeechBackend::onProcessError(QProcess::ProcessError error) {
    if (error == QProcess::FailedToStart) {
        failWith(tr("the local voice could not be started"));
    }
}

void KokoroSpeechBackend::onProcessFinished(int exitCode, QProcess::ExitStatus status) {
    m_ready = false;
    if (m_speaking && status == QProcess::NormalExit && exitCode != 0) {
        failWith(tr("the local voice stopped unexpectedly"));
    }
}

void KokoroSpeechBackend::failWith(const QString& reason) {
    m_ready = false;
    m_speaking = false;
    reportFailed(reason.isEmpty() ? tr("the local voice failed") : reason);
}

} // namespace kestrel::app
