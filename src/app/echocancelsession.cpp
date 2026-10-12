#include "app/echocancelsession.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QUuid>

namespace kestrel::app {
namespace {
QString quoted(const QString& text) {
    QJsonArray array;
    array.append(text);
    const QByteArray encoded = QJsonDocument(array).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(encoded.mid(1, encoded.size() - 2));
}

QString routeProperty(const QJsonObject& props, const char* key) {
    const auto value = props.value(QLatin1String(key));
    return value.isString() ? value.toString() : QString::number(value.toInt());
}
} // namespace

EchoCancelSession::EchoCancelSession(QObject* parent) : QObject(parent) {
    m_detail = tr("Echo cancellation idle; starts only when the microphone is requested.");
    m_prefix = QStringLiteral("kestrel-aec-%1-%2")
        .arg(QCoreApplication::applicationPid()).arg(QUuid::createUuid().toString(QUuid::Id128));
    m_poll.setInterval(500);
    m_deadline.setSingleShot(true);
    m_probeDeadline.setSingleShot(true);
    m_stopDeadline.setSingleShot(true);
    connect(&m_stopDeadline, &QTimer::timeout, this, [this] {
        if (m_filter.state() != QProcess::NotRunning) { m_filter.kill(); }
    });
    connect(&m_probeDeadline, &QTimer::timeout, this, [this] {
        if (m_starting || m_active) { fail(tr("PipeWire route inspection timed out. Capture stopped; check your user audio service and retry.")); }
    });
    connect(&m_probe, &QProcess::readyReadStandardOutput, this, [this] {
        if (m_probe.bytesAvailable() > 8 * 1024 * 1024 && (m_starting || m_active)) {
            fail(tr("PipeWire route information exceeded the safety limit. Capture stopped."));
        }
    });
    m_probe.setStandardErrorFile(QProcess::nullDevice());
    connect(&m_poll, &QTimer::timeout, this, &EchoCancelSession::inspect);
    connect(&m_deadline, &QTimer::timeout, this, [this] {
        fail(tr("PipeWire echo cancellation did not connect within 5 seconds. Check your default microphone/output and the WebRTC SPA plugin."));
    });
    connect(&m_probe, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &EchoCancelSession::inspectFinished);
    connect(&m_probe, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (m_starting || m_active) { fail(tr("Could not inspect PipeWire routes. Install pipewire-bin and retry.")); }
    });
    connect(&m_filter, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus) {
        if (m_starting || m_active) {
            fail(tr("The PipeWire echo-cancellation client stopped. Check that the WebRTC echo-cancel plugin is installed, then retry."));
        }
    });
    connect(&m_filter, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        if (m_starting || m_active) { fail(tr("Could not start the local PipeWire echo-cancellation client. Install pipewire-bin and retry.")); }
    });
}

EchoCancelSession::~EchoCancelSession() {
    stop();
    // Teardown only; normal UI cancellation is nonblocking.
    for (auto* process : {&m_probe, &m_filter}) {
        if (process->state() != QProcess::NotRunning && !process->waitForFinished(500)) {
            process->kill();
            process->waitForFinished(500);
        }
    }
}

bool EchoCancelSession::start(QString& error) {
#if defined(__linux__)
    if (m_active || m_starting || m_filter.state() != QProcess::NotRunning
        || m_probe.state() != QProcess::NotRunning) {
        error = tr("The echo-cancellation session is already running or stopping. Retry in a moment.");
        return false;
    }
    if (!m_configuration.isValid() || QStandardPaths::findExecutable(QStringLiteral("pipewire")).isEmpty()
        || QStandardPaths::findExecutable(QStringLiteral("pw-dump")).isEmpty()) {
        error = tr("Echo cancellation requires pipewire-bin, its WebRTC SPA plugin, and a writable temporary directory. Install the missing components and retry.");
        return false;
    }
    m_stopDeadline.stop();
    m_source.clear();
    m_sink.clear();
    m_starting = true;
    m_detail = tr("Connecting local WebRTC echo cancellation to the default microphone and output…");
    m_deadline.start(5000);
    emit changed();
    inspect();
    m_poll.start();
    return true;
#else
    error = tr("PipeWire echo cancellation is currently available on Linux only. Use headphones on this platform.");
    return false;
#endif
}

void EchoCancelSession::stop() {
    m_active = false;
    m_starting = false;
    m_poll.stop();
    m_deadline.stop();
    m_probeDeadline.stop();
    m_captureDevice.clear();
    m_probe.kill();
    m_filter.terminate();
    if (m_filter.state() != QProcess::NotRunning) { m_stopDeadline.start(500); }
    m_detail = tr("Echo cancellation idle; microphone capture is stopped.");
    emit changed();
}

void EchoCancelSession::fail(const QString& reason) {
    stop();
    m_detail = reason;
    emit changed();
    emit failed(reason);
}

void EchoCancelSession::inspect() {
    if ((!m_active && !m_starting) || m_probe.state() != QProcess::NotRunning) { return; }
    m_probe.setProgram(QStandardPaths::findExecutable(QStringLiteral("pw-dump")));
    m_probe.setArguments({QStringLiteral("--no-colors")});
    m_probe.start();
    m_probeDeadline.start(2000);
}

bool EchoCancelSession::launch(const QString& source, const QString& sink, QString& error) {
    // Monitor mode reads the actual default output, including Qt/platform TTS,
    // rather than guessing which PCM the speech engine will eventually render.
    // Both routes are pinned; a device/default change terminates the session.
    const QString config = QStringLiteral(
        "context.properties = { log.level = 0 }\n"
        "context.spa-libs = { audio.convert.* = audioconvert/libspa-audioconvert support.* = support/libspa-support }\n"
        "context.modules = [\n"
        " { name = libpipewire-module-protocol-native }\n"
        " { name = libpipewire-module-client-node }\n"
        " { name = libpipewire-module-adapter }\n"
        " { name = libpipewire-module-echo-cancel args = {\n"
        " library.name = aec/libspa-aec-webrtc monitor.mode = true\n"
        " audio.rate = 48000 audio.channels = 1 audio.position = [ MONO ]\n"
        " capture.props = { node.name = %1 target.object = %2 node.dont-fallback = true }\n"
        " sink.props = { node.name = %3 target.object = %4 node.dont-fallback = true resample.peaks = false }\n"
        " source.props = { node.name = %5 node.description = \"Kestrel echo-canceled microphone\" priority.session = 0 }\n"
        " } }\n]\n")
        .arg(quoted(m_prefix + QStringLiteral("-capture")), quoted(source),
             quoted(m_prefix + QStringLiteral("-reference")), quoted(sink),
             quoted(m_prefix + QStringLiteral("-clean")));
    const QString path = m_configuration.filePath(QStringLiteral("echo.conf"));
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(config.toUtf8()) != config.toUtf8().size()) {
        error = tr("Could not write the temporary echo-cancellation configuration. Check free space and temporary-folder permissions.");
        return false;
    }
    file.close();
    m_source = source;
    m_sink = sink;
    m_filter.setProgram(QStandardPaths::findExecutable(QStringLiteral("pipewire")));
    m_filter.setArguments({QStringLiteral("-c"), path});
    m_filter.setStandardOutputFile(QProcess::nullDevice());
    m_filter.setStandardErrorFile(QProcess::nullDevice());
    m_filter.start();
    return true;
}

void EchoCancelSession::inspectFinished(int code, QProcess::ExitStatus status) {
    m_probeDeadline.stop();
    if (!m_active && !m_starting) { return; }
    const QByteArray bytes = m_probe.readAllStandardOutput();
    if (bytes.size() > 8 * 1024 * 1024 || code != 0 || status != QProcess::NormalExit) {
        fail(tr("PipeWire route inspection failed. Start your user audio service and retry."));
        return;
    }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(bytes, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isArray()) {
        fail(tr("PipeWire returned invalid route information. Check the PipeWire installation."));
        return;
    }
    const auto objects = document.array();
    QString defaultSource, defaultSink;
    QJsonObject nodes;
    for (const auto& entry : objects) {
        const auto object = entry.toObject();
        const auto info = object.value(QStringLiteral("info")).toObject();
        const auto props = info.value(QStringLiteral("props")).toObject();
        if (object.value(QStringLiteral("type")).toString().endsWith(QStringLiteral(":Metadata"))
            && object.value(QStringLiteral("props")).toObject().value(QStringLiteral("metadata.name")).toString() == QStringLiteral("default")) {
            for (const auto& value : object.value(QStringLiteral("metadata")).toArray()) {
                const auto metadata = value.toObject();
                auto data = metadata.value(QStringLiteral("value"));
                const auto name = data.isObject() ? data.toObject().value(QStringLiteral("name")).toString()
                    : QJsonDocument::fromJson(data.toString().toUtf8()).object().value(QStringLiteral("name")).toString();
                const auto key = metadata.value(QStringLiteral("key")).toString();
                if (key == QStringLiteral("default.audio.source")) { defaultSource = name; }
                if (key == QStringLiteral("default.audio.sink")) { defaultSink = name; }
            }
        }
        if (object.value(QStringLiteral("type")).toString().endsWith(QStringLiteral(":Node"))) {
            nodes.insert(props.value(QStringLiteral("node.name")).toString(), object);
        }
    }
    const auto source = nodes.value(defaultSource).toObject();
    const auto sink = nodes.value(defaultSink).toObject();
    const auto hardware = [](const QJsonObject& node, const QString& mediaClass) {
        const auto props = node.value(QStringLiteral("info")).toObject().value(QStringLiteral("props")).toObject();
        return props.value(QStringLiteral("media.class")).toString() == mediaClass
            && !props.value(QStringLiteral("node.virtual")).toBool()
            && !props.value(QStringLiteral("device.api")).toString().isEmpty();
    };
    if (!hardware(source, QStringLiteral("Audio/Source")) || !hardware(sink, QStringLiteral("Audio/Sink"))) {
        fail(tr("Echo cancellation needs a default physical microphone and output. Select them in desktop Sound settings; virtual/monitor defaults are refused to avoid audio loops."));
        return;
    }
    if (m_source.isEmpty()) {
        QString error;
        if (!launch(defaultSource, defaultSink, error)) { fail(error); }
        return;
    }
    if (defaultSource != m_source || defaultSink != m_sink) {
        fail(tr("The default microphone or output changed. Capture stopped to avoid using a stale echo reference; press the microphone again."));
        return;
    }
    const auto id = [](const QJsonObject& node) { return node.value(QStringLiteral("id")).toInt(-1); };
    const auto capture = nodes.value(m_prefix + QStringLiteral("-capture")).toObject();
    const auto reference = nodes.value(m_prefix + QStringLiteral("-reference")).toObject();
    const auto clean = nodes.value(m_prefix + QStringLiteral("-clean")).toObject();
    bool captureLinked = false, referenceLinked = false;
    // If any playback stream uses a different sink, this reference cannot
    // cover it. Refuse rather than letting the app call an unrelated monitor safe.
    for (const auto& entry : objects) {
        const auto object = entry.toObject();
        const auto info = object.value(QStringLiteral("info")).toObject();
        if (!object.value(QStringLiteral("type")).toString().endsWith(QStringLiteral(":Link"))) { continue; }
        const int input = info.value(QStringLiteral("input-node-id")).toInt(-1);
        const int output = info.value(QStringLiteral("output-node-id")).toInt(-1);
        for (const auto& node : nodes) {
            const auto candidate = node.toObject();
            const auto candidateProps = candidate.value(QStringLiteral("info")).toObject().value(QStringLiteral("props")).toObject();
            if (candidate.value(QStringLiteral("id")).toInt(-1) == input && input != id(sink)
                && candidateProps.value(QStringLiteral("media.class")).toString() == QStringLiteral("Audio/Sink")
                && output >= 0) {
                fail(tr("Playback is routed to a different output than the echo reference. Route playback to the default output before simultaneous listening."));
                return;
            }
        }
    }
    for (const auto& entry : objects) {
        const auto info = entry.toObject().value(QStringLiteral("info")).toObject();
        const QString linkState = info.value(QStringLiteral("state")).toString();
        // Passive links are paused until the clean-source consumer starts.
        // They are negotiated routes, not a claim that PCM is already flowing.
        if (linkState != QStringLiteral("active") && linkState != QStringLiteral("paused")) { continue; }
        const auto output = info.value(QStringLiteral("output-node-id")).toInt(-1);
        const auto input = info.value(QStringLiteral("input-node-id")).toInt(-1);
        captureLinked = captureLinked || (output == id(source) && input == id(capture) && input >= 0);
        referenceLinked = referenceLinked || (output == id(sink) && input == id(reference) && input >= 0);
    }
    if (!captureLinked || !referenceLinked || clean.isEmpty()) {
        if (m_active) { fail(tr("The microphone or playback-reference link disappeared. Capture stopped; reconnect your audio devices and retry.")); }
        return;
    }
    if (!m_active) {
        const auto props = clean.value(QStringLiteral("info")).toObject().value(QStringLiteral("props")).toObject();
        m_captureDevice = QStringLiteral("pipewire:NODE=%1").arg(routeProperty(props, "object.serial"));
        m_starting = false;
        m_active = true;
        m_deadline.stop();
        m_detail = tr("Local WebRTC echo-cancel route connected: default microphone + output monitor. Other output routes are not covered; acoustic quality is not certified for this room.");
        emit changed();
        emit ready();
    }
}

} // namespace kestrel::app
