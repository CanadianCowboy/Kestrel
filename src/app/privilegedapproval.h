#pragma once

#include <QObject>
#include <QString>

#include "app/privilegedcommand.h"

class QTimer;

namespace kestrel::app {

// Connects a pending administrator request to the desktop UI.
//
// Only one request is pending at a time. A second request while one is waiting
// is refused at once, so an agent cannot queue up prompts or wear the user down
// into clicking Approve. An unanswered request is denied after a minute.
class PrivilegedApprovalBroker : public QObject {
    Q_OBJECT

public:
    explicit PrivilegedApprovalBroker(QObject* parent = nullptr);

    // Asks the user about `request` and reports the answer through `decide`.
    // Matches PrivilegedApproval, so it can be passed straight to
    // runPrivilegedCommand.
    void ask(const PrivilegedRequest& request, PrivilegedDecision decide);

    [[nodiscard]] bool hasPendingRequest() const noexcept { return m_decide != nullptr; }

public slots:
    // Called by the dialog. Answers only the request with this id, so a stale
    // dialog cannot approve a newer request.
    void respond(const QString& requestId, bool approved);

signals:
    void requestPending(const QString& requestId, const QString& program,
                        const QString& commandLine, const QString& reason);
    void requestResolved(const QString& requestId);

private:
    void resolve(bool approved);

    PrivilegedDecision m_decide;
    QString m_requestId;
    QTimer* m_timeout = nullptr;
    int m_nextId = 1;
};

} // namespace kestrel::app
