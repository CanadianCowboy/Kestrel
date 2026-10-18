#include "app/shellcommand.h"

#include <QDir>
#include <QProcess>
#include <QTimer>

#include <algorithm>
#include <memory>

namespace kestrel::app {
namespace {

// Long builds are allowed, but a command that never finishes must not hold the
// tool loop forever.
constexpr int kCommandTimeoutMs = 120000;
// Enough for a build log's tail, small enough to sit in one transcript row.
constexpr qsizetype kMaxOutputBytes = 16000;

} // namespace

ShellInvocation shellInvocation(const QString& commandLine) {
#ifdef Q_OS_WIN
    return {QStringLiteral("cmd.exe"), {QStringLiteral("/d"), QStringLiteral("/c"), commandLine}};
#else
    return {QStringLiteral("/bin/sh"), {QStringLiteral("-c"), commandLine}};
#endif
}

void runShellCommand(const QString& commandLine, std::function<void(core::McpToolResult)> done) {
    const ShellInvocation invocation = shellInvocation(commandLine);
    QProcess* process = new QProcess();
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setWorkingDirectory(QDir::homePath());

    // Every path that can end the run (exit, failure to start, timeout) funnels
    // through here, and only the first one reports.
    auto reported = std::make_shared<bool>(false);
    auto capturedOutput = std::make_shared<QByteArray>();
    auto outputTruncated = std::make_shared<bool>(false);
    auto captureOutput = [process, capturedOutput, outputTruncated] {
        const QByteArray chunk = process->readAllStandardOutput();
        const qsizetype remaining = kMaxOutputBytes - capturedOutput->size();
        capturedOutput->append(chunk.constData(), std::min(chunk.size(), remaining));
        if (chunk.size() > remaining) {
            *outputTruncated = true;
        }
    };
    QObject::connect(process, &QProcess::readyReadStandardOutput, process, captureOutput);
    auto complete = [process, done = std::move(done), reported](core::McpToolResult result) {
        if (*reported) {
            return;
        }
        *reported = true;
        done(std::move(result));
        process->deleteLater();
    };

    QObject::connect(process, &QProcess::errorOccurred, process,
                     [complete, process](QProcess::ProcessError error) {
                         if (error == QProcess::FailedToStart) {
                             complete(core::McpToolResult{
                                 true, "The shell could not start: " + process->errorString().toStdString()});
                         }
                     });
    QObject::connect(process, &QProcess::finished, process,
                     [complete, captureOutput, capturedOutput, outputTruncated, process](
                         int exitCode, QProcess::ExitStatus status) {
                         captureOutput();
                         QString text = QStringLiteral("exit code %1").arg(exitCode);
                         if (status == QProcess::CrashExit) {
                             text = QStringLiteral("the command was stopped before it finished");
                         }
                         text += QLatin1Char('\n') + QString::fromUtf8(*capturedOutput);
                         if (*outputTruncated) {
                             text += QStringLiteral("\n[output cut short]");
                         }
                         complete(core::McpToolResult{exitCode != 0 || status == QProcess::CrashExit,
                                                      text.trimmed().toStdString()});
                     });
    QTimer::singleShot(kCommandTimeoutMs, process, [complete, process] {
        if (process->state() != QProcess::NotRunning) {
            process->kill();
        }
        complete(core::McpToolResult{true, "The command ran longer than two minutes and was stopped."});
    });

    process->start(invocation.program, invocation.arguments);
}

} // namespace kestrel::app
