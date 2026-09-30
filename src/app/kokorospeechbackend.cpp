#include "app/kokorospeechbackend.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace kestrel::app {

/// Stores the driver paths and optionally connects and starts the Kokoro process.
KokoroSpeechBackend::KokoroSpeechBackend(QString python, QString serverScript,
                                         bool startImmediately, QObject* parent)
    : LocalModelSpeechBackend(parent),
      m_python(std::move(python)),
      m_serverScript(std::move(serverScript)) {
    if (!startImmediately) {
        return;
    }
    m_process = new QProcess(this);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    connect(m_process, &QProcess::readyReadStandardOutput, this,
            &KokoroSpeechBackend::onReadyRead);
    connect(m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart || error == QProcess::Crashed) {
            noteEngineFailed(tr("the local voice could not be started"));
        }
    });
    connect(m_process, &QProcess::started, this, [this] { markLaunched(); });
    connect(m_process, &QProcess::finished, this, [this](int, QProcess::ExitStatus) {
        noteEngineFailed(tr("the local voice process exited"));
    });
    completeSetup();
}

/// Returns the supported Kokoro voice identifiers in preference order.
QStringList KokoroSpeechBackend::engineVoices() const {
    // Listed rather than discovered at runtime: the voice table is a file on
    // disk, and parsing it to build a picker would be a second source of truth
    // to keep in step with the model's contents.
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

/// Launches the driver asynchronously; process signals publish launch or failure.
void KokoroSpeechBackend::startEngine() {
    if (m_python.isEmpty() || m_serverScript.isEmpty()) {
        // Nothing to launch, so nothing will ever answer. Said out loud, because
        // a response waiting on this voice has to stop waiting rather than be
        // held for an engine that was never coming.
        markGivenUp();
        return;
    }
    m_process->start(m_python, {m_serverScript});

}

/// Writes a JSON synthesis request containing the text, voice, speed, and output path.
void KokoroSpeechBackend::synthesise(const QString& text, const QString& path) {
    QJsonObject request;
    request.insert(QStringLiteral("text"), text);
    request.insert(QStringLiteral("voice"), currentVoice());
    request.insert(QStringLiteral("speed"), speedForRequest());
    request.insert(QStringLiteral("out"), path);
    m_process->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n");
}

/// Consumes complete driver replies, reporting readiness, synthesized files, or engine failure.
void KokoroSpeechBackend::onReadyRead() {
    while (m_process->canReadLine()) {
        const QByteArray line = m_process->readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(line, &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
            continue;
        }
        const QJsonObject reply = document.object();
        if (reply.value(QStringLiteral("ready")).toBool()) {
            // The driver announcing itself is the one moment this engine is
            // known to be able to answer. It used to be discarded here and the
            // voice was reported ready the moment the process spawned, which is
            // earlier than anything can actually speak -- so a reply asked for
            // during the load was handed audio to an engine that could not make
            // it, and a reply asked for when the process failed was delivered as
            // text with nothing ever arriving to change that.
            markStarted();
            continue;
        }
        if (!reply.value(QStringLiteral("ok")).toBool()) {
            noteClauseFailed(reply.value(QStringLiteral("error")).toString());
            continue;
        }
        noteSynthesised(reply.value(QStringLiteral("out")).toString());
    }
}

} // namespace kestrel::app
