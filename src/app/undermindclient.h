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

// The OAuth Client ID Metadata Document URL for this build's platform.
[[nodiscard]] QString undermindClientId();
[[nodiscard]] QString kestrelCimdClientId();

// Finds a JSON-RPC response by ID in JSON or an SSE response body.
[[nodiscard]] std::optional<QJsonObject> jsonRpcResponse(const QByteArray& body,
                                                         const QByteArray& contentType, std::int64_t id);

// Configuration for any remote Streamable HTTP MCP server. Defaults to
// Undermind, while allowing custom servers to use CIMD + RFC 9728 / RFC 8414
// OAuth discovery or a pre-configured Bearer token.
struct McpServerConfig {
    QString id;              // Short slug used for tool prefixing and keystore account
    QString displayName;     // Human-readable name (e.g., "Undermind")
    QString mcpUrl;          // Streamable HTTP MCP endpoint URL
    QString resource;        // Canonical RFC 8707 resource URI (defaults to mcpUrl)
    QString authorizeUrl;    // Optional explicit OAuth authorize URL (discovered via RFC 9728/8414 if empty)
    QString tokenUrl;        // Optional explicit OAuth token URL (discovered via RFC 9728/8414 if empty)
    QString scope;           // Optional OAuth scope (defaults to "mcp" or discovered scopes)
    QString clientId;        // Optional override (defaults to kestrelCimdClientId())
    QString bearerToken;     // Optional static Bearer token (skips OAuth when non-empty)

    [[nodiscard]] static McpServerConfig undermindDefault();
};

// Connects Kestrel to a remote MCP endpoint (Undermind or any added server).
// Supports OAuth 2.1 + PKCE with Client ID Metadata Documents (CIMD),
// RFC 9728 Protected Resource Metadata discovery, RFC 8414 / OIDC Authorization
// Server Metadata discovery, and static Bearer tokens. Keeps credentials in the
// platform keystore, lists the server's tools, resources, and prompts, and
// forwards calls. Every reply completes exactly once.
class UndermindClient final : public QObject {
    Q_OBJECT

public:
    explicit UndermindClient(QObject* parent = nullptr);
    explicit UndermindClient(McpServerConfig config, QObject* parent = nullptr);
    // Injectable transport and storage for protocol tests. The network must
    // outlive this client; the normal constructor always uses the OS keystore.
    UndermindClient(QNetworkAccessManager& network, std::unique_ptr<storage::SecretStore> store,
                    QObject* parent = nullptr);
    UndermindClient(McpServerConfig config, QNetworkAccessManager& network,
                    std::unique_ptr<storage::SecretStore> store, QObject* parent = nullptr);
    ~UndermindClient() override;

    [[nodiscard]] const McpServerConfig& config() const noexcept { return m_config; }

    // Signs in if needed, then lists the tools (and optional resources/prompts).
    // `done` receives the tools on success, and an empty array when failed.
    void connectAndList(std::function<void(bool ok, QJsonArray tools)> done);

    // True when a saved access/refresh token or static bearer token is ready
    // for this client_id and MCP resource.
    [[nodiscard]] bool hasSavedTokens();

    // Connects and lists tools using only saved tokens (and refresh_token if
    // expired), without opening a browser if no valid token is available.
    void connectSavedAndList(std::function<void(bool ok, QJsonArray tools)> done);

    // Runs one tool on the server. Arguments are the JSON the model wrote.
    void callTool(const std::string& name, const std::string& argumentsJson,
                  core::McpCompletion done);

    // Reads a resource or gets a prompt on the remote MCP server when supported.
    void readResource(const QString& uri, std::function<void(core::McpToolResult)> done);
    void getPrompt(const QString& name, const QJsonObject& arguments,
                   std::function<void(core::McpToolResult)> done);

    [[nodiscard]] QJsonArray discoveredTools() const { return m_tools; }
    [[nodiscard]] QJsonArray discoveredResources() const { return m_resources; }
    [[nodiscard]] QJsonArray discoveredPrompts() const { return m_prompts; }
    [[nodiscard]] QJsonObject serverCapabilities() const { return m_serverCapabilities; }

    [[nodiscard]] QString status() const { return m_status; }
    // True only after authenticated initialization and valid tool discovery.
    [[nodiscard]] bool connected() const { return m_connected; }

signals:
    void statusChanged();

private:
    struct HttpResult {
        int status = 0;
        QByteArray body;
        QByteArray contentType;
        QByteArray session;
        QByteArray wwwAuthenticate;
        QString error;
    };

    using Done = std::function<void(bool ok, QString error)>;

    void setStatus(const QString& status);
    void disconnectSession(const QString& reason);
    void connectAndListImpl(bool allowInteractiveSignIn,
                            std::function<void(bool ok, QJsonArray tools)> done);
    void discoverOptionalCapabilities(const QJsonArray& tools,
                                      std::function<void(bool ok, QJsonArray tools)> done);
    void ensureToken(bool allowInteractiveSignIn, Done done);
    void ensureOAuthEndpoints(Done done);
    void discoverProtectedResource(const QList<QUrl>& candidates, int index, Done done);
    void discoverAuthServerMetadata(const QUrl& issuer, const QList<QUrl>& candidates, int index,
                                    const QStringList& resourceScopes, Done done);
    void startSignIn(Done done);
    void finishSignIn(bool ok, const QString& error);
    void closeListener();
    void requestTokens(const QUrlQuery& form, Done done);
    void postMcp(const QJsonObject& body, std::function<void(HttpResult)> done);
    void getJson(const QUrl& url, std::function<void(int status, QJsonObject object, QString error)> done);
    void loadTokens();
    [[nodiscard]] bool saveTokens(QString& error);
    void forgetTokens();
    [[nodiscard]] QString effectiveClientId() const;
    [[nodiscard]] QString effectiveResource() const;
    [[nodiscard]] QString keystoreAccount() const;

    McpServerConfig m_config;
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
    QString m_discoveredIssuer;
    bool m_requireIssuerInAuthResponse = false;
    QByteArray m_session;
    QJsonObject m_serverCapabilities;
    QJsonArray m_tools;
    QJsonArray m_resources;
    QJsonArray m_prompts;
    std::int64_t m_nextId = 1;
    QString m_status;
    bool m_connected = false;
    bool m_connecting = false;
};

using McpRemoteClient = UndermindClient;

} // namespace kestrel::app
