#pragma once

#include <QString>
#include <QStringList>

#include <functional>

#include "core/mcpdispatch.h"

namespace kestrel::app {

// The program and arguments that run one command line in the ordinary shell.
struct ShellInvocation {
    QString program;
    QStringList arguments;
};

[[nodiscard]] ShellInvocation shellInvocation(const QString& commandLine);

// Runs one command line in the home directory and reports its exit code and
// output. The output is capped and the run is stopped after two minutes. This
// does not ask the user; the caller must already have the user's approval.
void runShellCommand(const QString& commandLine, std::function<void(core::McpToolResult)> done);

} // namespace kestrel::app
