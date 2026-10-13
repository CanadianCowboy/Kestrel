#pragma once

#include <QObject>
#include <QString>

#include "core/mcpdispatch.h"

class QTcpServer;

namespace kestrel::app {

// Serves the MCP endpoint in the background of the running app.
//
// This class only moves bytes. Framing, the Host check, the bearer token, and
// the JSON-RPC semantics all live in core so they are tested without a socket.
// The server binds to the loopback interface only: a remote machine cannot
// reach it, and a page in a browser can only reach it through the Host guard.
class McpServer : public QObject {
    Q_OBJECT

public:
    // The dispatcher is owned by the caller and must outlive the server. Every
    // request is answered on the GUI thread, so tool handlers run there too.
    McpServer(core::McpDispatcher& dispatcher, QString token, QObject* parent = nullptr);
    ~McpServer() override;

    // Starts listening on 127.0.0.1. Returns false and sets `error` with what
    // to do next (usually: pick another port, or stop the other program).
    [[nodiscard]] bool start(quint16 port, QString& error);
    [[nodiscard]] quint16 port() const;
    [[nodiscard]] bool isListening() const;

private:
    void acceptConnections();

    core::McpDispatcher& m_dispatcher;
    QString m_token;
    QTcpServer* m_server = nullptr;
};

} // namespace kestrel::app
