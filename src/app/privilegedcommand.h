#pragma once

#include <functional>

#include <QString>
#include <QStringList>

namespace kestrel::app {

// One command that wants administrator rights.
//
// The program and its arguments are kept separate and handed to pkexec as an
// argument vector. Nothing is passed through a shell, so a crafted argument
// cannot add a command, and the authentication itself is left to polkit: the
// password is typed into the desktop's own dialog and never reaches Kestrel.
struct PrivilegedRequest {
    QString program;
    QStringList arguments;
    // Shown to the user in the approval prompt, so they can judge the request
    // rather than trust the agent that made it.
    QString reason;
};

struct PrivilegedResult {
    bool ran = false;
    int exitCode = -1;
    QString output;
    // What the user should do next when the command did not run.
    QString error;
};

using PrivilegedCompletion = std::function<void(PrivilegedResult)>;
// Asks a person about one request and reports the answer through `decide`.
// The answer may arrive later, after this call returns.
using PrivilegedDecision = std::function<void(bool approved)>;
using PrivilegedApproval = std::function<void(const PrivilegedRequest&, PrivilegedDecision decide)>;

// Runs one approved command through pkexec, then reports through `done`.
//
// Refusal is the default: without an approver, or when the approver says no,
// nothing is executed and `done` reports why. `done` is called exactly once.
void runPrivilegedCommand(const PrivilegedRequest& request, const PrivilegedApproval& approve,
                          PrivilegedCompletion done);

} // namespace kestrel::app
