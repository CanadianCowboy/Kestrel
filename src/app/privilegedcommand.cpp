#include "app/privilegedcommand.h"

#include <memory>

#include <QProcess>
#include <QStandardPaths>
#include <QTimer>

namespace kestrel::app {

namespace {

// A privileged command that has not finished in a minute is almost always
// waiting on a dialog nobody can see, so Kestrel stops waiting rather than
// leaving the request open indefinitely.
constexpr int kPrivilegedTimeoutMs = 60000;

PrivilegedResult refusal(QString error) {
    PrivilegedResult result;
    result.error = std::move(error);
    return result;
}

// Starts pkexec and reports once, when the process has finished or been
// stopped. The process is owned by this function and removed after reporting.
void startPkexec(const PrivilegedRequest& request, const QString& pkexec,
                 const PrivilegedCompletion& done) {
    auto* process = new QProcess();
    process->setProcessChannelMode(QProcess::MergedChannels);
    QStringList argv;
    argv << request.program << request.arguments;
    process->setProgram(pkexec);
    process->setArguments(argv);

    // Shared so exactly one of finished / error / timeout reports.
    auto reported = std::make_shared<bool>(false);
    auto report = [process, reported, done](PrivilegedResult result) {
        if (*reported) {
            return;
        }
        *reported = true;
        done(std::move(result));
        process->deleteLater();
    };

    auto* timer = new QTimer(process);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, process, [process, report] {
        PrivilegedResult result;
        result.output = QString::fromLocal8Bit(process->readAll());
        result.error = QStringLiteral("The administrator command did not finish within a minute. Check whether a password dialog is waiting on your desktop, then try again.");
        report(std::move(result));
        process->kill();
    });

    QObject::connect(process, &QProcess::errorOccurred, process,
                     [process, report](QProcess::ProcessError) {
                         // Start failures arrive here before finished would fire.
                         if (process->state() == QProcess::NotRunning) {
                             report(refusal(QStringLiteral("pkexec could not start: %1").arg(process->errorString())));
                         }
                     });
    QObject::connect(process, &QProcess::finished, process,
                     [process, report, timer](int exitCode, QProcess::ExitStatus status) {
                         timer->stop();
                         PrivilegedResult result;
                         result.ran = true;
                         result.exitCode = exitCode;
                         result.output = QString::fromLocal8Bit(process->readAll());
                         if (status != QProcess::NormalExit) {
                             result.error = QStringLiteral("The administrator command was stopped before it finished.");
                         } else if (exitCode == 126) {
                             // pkexec reports 126 when authentication is dismissed or refused.
                             result.error = QStringLiteral("Administrator authentication was cancelled or refused, so the command did not run.");
                         } else if (exitCode == 127) {
                             result.error = QStringLiteral("The requested program was not found with administrator rights.");
                         }
                         report(std::move(result));
                     });

    timer->start(kPrivilegedTimeoutMs);
    process->start();
}

} // namespace

void runPrivilegedCommand(const PrivilegedRequest& request, const PrivilegedApproval& approve,
                          PrivilegedCompletion done) {
    if (request.program.isEmpty() || request.program.contains(QLatin1Char('\0'))) {
        done(refusal(QStringLiteral("The command needs a program name to run.")));
        return;
    }
    if (!approve) {
        done(refusal(QStringLiteral("The administrator command was not approved, so it did not run.")));
        return;
    }

    approve(request, [request, done](bool approved) {
        if (!approved) {
            done(refusal(QStringLiteral("The administrator command was not approved, so it did not run.")));
            return;
        }
        // The host's own authentication agent asks for the password. A silent
        // fallback to sudo with a typed password would bypass the desktop dialog
        // this path is built around, so a missing pkexec is reported instead.
        const QString pkexec = QStandardPaths::findExecutable(QStringLiteral("pkexec"));
        if (pkexec.isEmpty()) {
            done(refusal(QStringLiteral("pkexec was not found. Install polkit (for example the \"policykit-1\" package on Ubuntu) and try again.")));
            return;
        }
        startPkexec(request, pkexec, done);
    });
}

} // namespace kestrel::app
