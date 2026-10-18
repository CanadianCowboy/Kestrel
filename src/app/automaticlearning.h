#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <memory>

class QTemporaryDir;

namespace kestrel::app {

// App-owned agent work, never an IdlePersona capability. Runs only an installed
// local trainer/converter, without a shell, downloads, or network credentials.
class AutomaticLearning final : public QObject {
    Q_OBJECT
public:
    explicit AutomaticLearning(QObject* parent = nullptr);
    ~AutomaticLearning() override;
    [[nodiscard]] bool busy() const { return m_busy; }
    [[nodiscard]] QString status() const { return m_status; }
    [[nodiscard]] QString availabilityError() const;
    void start(const QByteArray& dataset, const QString& inferenceModel, const QString& stage = QStringLiteral("persona"));
    void cancel();

signals:
    void changed();
    // Empty adapter means the previous model/adapter should be restored.
    void finished(QString adapter, QString message);

private:
    void complete(const QString& adapter, const QString& message);
    void processFinished(int code, QProcess::ExitStatus exitStatus);
    QString m_python;
    QString m_trainer;
    QString m_model;
    QString m_config;
    QString m_converter;
    QString m_output;
    QString m_status = QStringLiteral("Waiting for permission to learn from chat.");
    QProcess m_process;
    std::unique_ptr<QTemporaryDir> m_dataset;
    QByteArray m_log;
    QString m_stage = QStringLiteral("persona");
    bool m_busy = false;
    bool m_converting = false;
    bool m_cancelled = false;
};

} // namespace kestrel::app
