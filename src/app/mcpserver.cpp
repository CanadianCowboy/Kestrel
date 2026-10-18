#include "app/mcpserver.h"

#include <memory>
#include <string>

#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

#include "core/mcphttp.h"

namespace kestrel::app {

namespace {

// A client that opens a connection and never finishes its request must not
// hold a socket forever.
constexpr int kRequestTimeoutMs = 10000;

const char* reasonPhrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    default: return "Internal Server Error";
    }
}

std::string serialise(const core::McpHttpReply& reply) {
    std::string wire = "HTTP/1.1 " + std::to_string(reply.status) + " " + reasonPhrase(reply.status) + "\r\n";
    if (!reply.contentType.empty()) {
        wire += "Content-Type: " + reply.contentType + "\r\n";
    }
    if (!reply.sessionId.empty()) {
        wire += "Mcp-Session-Id: " + reply.sessionId + "\r\n";
    }
    wire += "Content-Length: " + std::to_string(reply.body.size()) + "\r\n";
    wire += "Connection: close\r\n\r\n";
    wire += reply.body;
    return wire;
}

} // namespace

McpServer::McpServer(core::McpDispatcher& dispatcher, QString token, QObject* parent)
    : QObject(parent)
    , m_dispatcher(dispatcher)
    , m_token(std::move(token))
    , m_server(new QTcpServer(this)) {
    connect(m_server, &QTcpServer::newConnection, this, &McpServer::acceptConnections);
}

McpServer::~McpServer() = default;

bool McpServer::start(quint16 port, QString& error) {
    if (m_server->isListening()) {
        m_server->close();
    }
    // Loopback only. Binding to Any would expose the endpoint to the network,
    // and the bearer token is not a reason to make that a safe default.
    if (!m_server->listen(QHostAddress::LocalHost, port)) {
        error = QStringLiteral("Kestrel could not listen on 127.0.0.1:%1 (%2). Choose another port, or stop the program using it.")
                    .arg(port)
                    .arg(m_server->errorString());
        return false;
    }
    error.clear();
    return true;
}

void McpServer::stop() {
    if (m_server != nullptr && m_server->isListening()) {
        m_server->close();
    }
}

quint16 McpServer::port() const {
    return m_server->serverPort();
}

bool McpServer::isListening() const {
    return m_server->isListening();
}

void McpServer::acceptConnections() {
    while (m_server->hasPendingConnections()) {
        QTcpSocket* socket = m_server->nextPendingConnection();
        // Shared so the lambda that reads and the one that replies see the same
        // bytes; the socket itself is parented to the server and freed on close.
        auto buffer = std::make_shared<std::string>();
        auto answered = std::make_shared<bool>(false);

        auto timeout = new QTimer(socket);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, socket, [socket, answered] {
            if (!*answered) {
                *answered = true;
                socket->disconnectFromHost();
            }
        });
        timeout->start(kRequestTimeoutMs);

        connect(socket, &QTcpSocket::readyRead, this, [this, socket, buffer, answered] {
            if (*answered) {
                return;
            }
            buffer->append(socket->readAll().toStdString());
            if (!core::mcpRequestComplete(*buffer)) {
                return;
            }
            *answered = true;
            // The reply may come much later, when a person has decided an
            // administrator request. The client may leave in the meantime, so
            // the write goes through a guard rather than a bare pointer.
            QPointer<QTcpSocket> alive(socket);
            core::answerMcpRequestAsync(*buffer, m_token.toStdString(), m_dispatcher,
                                        [alive](core::McpHttpReply reply) {
                                            if (!alive) {
                                                return;
                                            }
                                            alive->write(QByteArray::fromStdString(serialise(reply)));
                                            alive->disconnectFromHost();
                                        });
        });
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }
}

} // namespace kestrel::app
