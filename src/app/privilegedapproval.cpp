#include "app/privilegedapproval.h"

#include <QTimer>

namespace kestrel::app {

namespace {

// Matches the runner's own timeout: the user has the same minute to answer.
constexpr int kApprovalTimeoutMs = 60000;

} // namespace

PrivilegedApprovalBroker::PrivilegedApprovalBroker(QObject* parent)
    : QObject(parent)
    , m_timeout(new QTimer(this)) {
    m_timeout->setSingleShot(true);
    connect(m_timeout, &QTimer::timeout, this, [this] { resolve(false); });
}

void PrivilegedApprovalBroker::ask(const PrivilegedRequest& request, PrivilegedDecision decide) {
    if (m_decide) {
        // One at a time. Refusing here is safer than queueing prompts.
        decide(false);
        return;
    }
    m_decide = std::move(decide);
    m_requestId = QStringLiteral("admin-%1").arg(m_nextId++);

    QStringList parts;
    parts << request.program << request.arguments;
    emit requestPending(m_requestId, request.program, parts.join(QLatin1Char(' ')), request.reason);
    m_timeout->start(kApprovalTimeoutMs);
}

void PrivilegedApprovalBroker::respond(const QString& requestId, bool approved) {
    if (!m_decide || requestId != m_requestId) {
        return; // stale dialog, or nothing waiting
    }
    resolve(approved);
}

void PrivilegedApprovalBroker::resolve(bool approved) {
    // Taken before the callback runs, so re-entrant calls see no pending request.
    PrivilegedDecision decide = std::move(m_decide);
    m_decide = nullptr;
    const QString id = m_requestId;
    m_requestId.clear();
    m_timeout->stop();
    emit requestResolved(id);
    decide(approved);
}

} // namespace kestrel::app
