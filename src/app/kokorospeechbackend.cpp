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
    // A driver that dies *after* starting is the case that used to be
    // invisible. QProcess::FailedToStart only covers the launch, and a process
    // that starts and then exits -- a Python traceback, a CUDA failure, the
    // machine suspending -- raised nothing at all. m_started stayed true, the
    // next speak() wrote into a process that was already gone, and because the
    // reply never came, speakingNow() never cleared and the clause pump waited
    // for a completion that had no way of arriving. The reply was neither
    // spoken nor delivered as text; it just stopped, mid-pump, forever.
    //
    // Any exit that was not asked for is therefore an engine failure, reported
    // through the same path as a launch failure so the caller unwinds the same
    // way.
    connect(m_process, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        // This persistent driver has no normal shutdown while owned. Even a
        // zero exit is unexpected and must release any reply waiting for it.
        static_cast<void>(status);
        noteEngineFailed(tr("the local voice stopped unexpectedly (exit %1)")
                             .arg(exitCode));
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
        // Nothing to launch, so nothing will ever answer. Said out loud, because
        // a response waiting on this voice has to stop waiting rather than be
        // held for an engine that was never coming.
        markGivenUp();
        return;
    }
    m_process->start(m_python, {m_serverScript});
    // Launched, not ready. The model is still loading at this point, which is
    // deliberate: the driver answers requests only once it has the model, and
    // queues the ones that arrive before that rather than dropping them. The
    // voice becomes usable when the driver announces itself, not here.
    //
    // No waitForStarted here, and that is the whole point. This runs from the
    // KokoroSpeechBackend constructor, which LocalVoiceEngines::create() calls
    // and the AppController calls on the UI thread -- so a synchronous wait is a
    // frozen window for as long as it allows, at start-up and on every engine
    // switch. A slow disk or a virtual environment inside a syncing folder is
    // enough to spend all of it.
    //
    // A launch that fails is not lost by not waiting for it: the
    // errorOccurred(FailedToStart) handler above reports it, and markGivenUp()
    // now withdraws the promise for an engine that was launched and then died,
    // which is what releases a reply that is being held for this voice. The
    // answer just arrives asynchronously rather than being paid for up front.
    markLaunched();
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
            noteEngineFailed(reply.value(QStringLiteral("error")).toString());
            continue;
        }
        noteSynthesised(reply.value(QStringLiteral("out")).toString());
    }
}

} // namespace kestrel::app
