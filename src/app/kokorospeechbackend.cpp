#include "app/kokorospeechbackend.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>

namespace kestrel::app {

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
        if (error == QProcess::FailedToStart) {
            noteEngineFailed(tr("the local voice could not be started"));
        }
    });
    completeSetup();
}

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

void KokoroSpeechBackend::startEngine() {
    if (m_python.isEmpty() || m_serverScript.isEmpty()) {
        return;
    }
    m_process->start(m_python, {m_serverScript});
    if (!m_process->waitForStarted(5000)) {
        return;
    }
    // The model is still loading at this point, which is deliberate: the server
    // reads requests only once it is ready, so a clause sent during the load is
    // queued and answered a moment later rather than lost.
    markStarted();
}

void KokoroSpeechBackend::synthesise(const QString& text, const QString& path) {
    QJsonObject request;
    request.insert(QStringLiteral("text"), text);
    request.insert(QStringLiteral("voice"), currentVoice());
    request.insert(QStringLiteral("speed"), speedForRequest());
    request.insert(QStringLiteral("out"), path);
    m_process->write(QJsonDocument(request).toJson(QJsonDocument::Compact) + "\n");
}

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
            continue;
        }
        if (!reply.value(QStringLiteral("ok")).toBool()) {
            noteEngineFailed(reply.value(QStringLiteral("error")).toString());
            continue;
        }
        noteSynthesised(reply.value(QStringLiteral("out")).toString());
    }
}

} // namespace kestrel::app
