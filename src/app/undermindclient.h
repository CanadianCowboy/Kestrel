#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QUrlQuery>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "core/mcpdispatch.h"
#include "storage/secretstore.h"

class QNetworkAccessManager;
class QTcpServer;

namespace kestrel::app {

// The PKCE code challenge for a verifier (RFC 7636, method S256).
[[nodiscard]] QString pkceChallenge(const QString& verifier);

// A fresh 64-character verifier from the unreserved set, drawn from the
// system's cryptographic generator.
[[nodiscard]] QString pkceVerifier();

// The JSON-RPC response with `id` from a reply body. The body is either one
// JSON object, or a server-sent event stream where the answer is in a data line.
[[nodiscard]] std::optional<QJsonObject> jsonRpcResponse(const QByteArray& body,
                                                         const QByteArray& contentType,
                                                         std::int64_t id);

// Connects Kestrel to Undermind's MCP endpoint. It signs in with OAuth (PKCE,
// loopback redirect), keeps the tokens in the platform keystore, lists the
// server's tools, and forwards calls. Every reply completes exactly once.
class UndermindClient final : public QObject {
    Q_OBJECT

public:
    explicit UndermindClient(QObject* parent = nullptr);
    ~UndermindClient() override;

    // Signs in if needed, then lists the tools. `done` receives the tools on
    // success, and an empty array when the connection failed.
    void connectAndList(std::function<void(bool ok, QJsonArray tools)> done);

    // Runs one tool on the server. Arguments are the JSON the model wrote.
    void callTool(const std::string& name, const std::string& argumentsJson,
                  core::McpCompletion done);

    [[nodiscard]] QString status() const { return m_status; }

signals:
    void statusChanged();

private:
    struct HttpResult {
        int status = 0;
        QByteArray body;
        QByteArray contentType;
        QByteArray session;
        QString error;
    };

    using Done = std::function<void(bool ok, QString error)>;

    void setStatus(const QString& status);
    void ensureToken(Done done);
    void startSignIn(Done done);
    void finishSignIn(bool ok, const QString& error);
    void closeListener();
    void requestTokens(const QUrlQuery& form, Done done);
    void postMcp(const QJsonObject& body, std::function<void(HttpResult)> done);
    void loadTokens();
    [[nodiscard]] bool saveTokens(QString& error);
    void forgetTokens();

    QNetworkAccessManager* m_network = nullptr;
    std::unique_ptr<storage::SecretStore> m_store;

    QTcpServer* m_listener = nullptr;
    QString m_verifier;
    QString m_state;
    Done m_signInDone;
    std::uint64_t m_signInAttempt = 0;

    QString m_accessToken;
    QString m_refreshToken;
    std::int64_t m_expiresAtMs = 0;
    QByteArray m_session;
    std::int64_t m_nextId = 1;
    QString m_status;
};

} // namespace kestrel::app
