#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <QTemporaryDir>
#include <QTimer>

namespace kestrel::app {

// Optional Linux adapter. Owns a temporary PipeWire client, never changes the
// user's audio defaults/configuration, and only opens capture after start().
// The default output monitor supplies the actual rendered playback reference.
class EchoCancelSession final : public QObject {
    Q_OBJECT
public:
    explicit EchoCancelSession(QObject* parent = nullptr);
    ~EchoCancelSession() override;
    bool start(QString& error);
    void stop();
    [[nodiscard]] bool active() const noexcept { return m_active; }
    [[nodiscard]] bool starting() const noexcept { return m_starting; }
    [[nodiscard]] QString captureDevice() const { return m_captureDevice; }
    [[nodiscard]] QString detail() const { return m_detail; }

signals:
    void ready();
    void failed(const QString& reason);
    void changed();

private:
    void inspect();
    void inspectFinished(int code, QProcess::ExitStatus status);
    void fail(const QString& reason);
    bool launch(const QString& source, const QString& sink, QString& error);
    QProcess m_filter;
    QProcess m_probe;
    QTimer m_poll;
    QTimer m_deadline;
    QTimer m_probeDeadline;
    QTimer m_stopDeadline;
    QTemporaryDir m_configuration;
    QString m_prefix;
    QString m_source;
    QString m_sink;
    QString m_captureDevice;
    QString m_detail;
    bool m_starting = false;
    bool m_active = false;
};

} // namespace kestrel::app
