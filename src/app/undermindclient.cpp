#include "app/undermindclient.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <utility>
#include <vector>

namespace kestrel::app {
namespace {

// The client ID is the public HTTPS URL of this platform's OAuth Client ID
// Metadata Document (CIMD), which MCP authorization servers fetch to learn
// Kestrel's name and loopback redirect before the user approves access.
#if defined(_WIN32)
constexpr char kClientId[] = "https://identity.josh-lynes.com/oauth/kestrel-windows.json";
#elif defined(__linux__)
constexpr char kClientId[] = "https://identity.josh-lynes.com/oauth/kestrel-linux.json";
#else
constexpr char kClientId[] =
    "https://raw.githubusercontent.com/CanadianCowboy/Kestrel/feature/cross-os-persona/docs/mcp/kestrel-oauth-client.json";
#endif
constexpr char kAuthorizeUrl[] = "https://api.undermind.ai/o/authorize/";
constexpr char kTokenUrl[] = "https://api.undermind.ai/o/token/";
constexpr char kMcpUrl[] = "https://mcp.undermind.ai/mcp";
constexpr char kResource[] = "https://mcp.undermind.ai/mcp";
constexpr char kRedirectUri[] = "http://127.0.0.1:8787/callback";
constexpr std::uint16_t kRedirectPort = 8787;
constexpr char kAccount[] = "undermind";
constexpr char kService[] = "com.kestrel.mcp";
constexpr char kProtocolVersion[] = "2025-06-18";
// Deep searches take minutes, so a tool call gets a generous deadline.
constexpr int kToolTimeoutMs = 300000;
constexpr int kSignInTimeoutMs = 300000;
// Refresh a little before expiry rather than discovering it mid-call.
constexpr std::int64_t kRefreshMarginMs = 60000;
constexpr char kUnreserved[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";

QJsonObject rpcRequest(std::int64_t id, const QString& method, const QJsonObject& params) {
    QJsonObject request;
    request.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
    request.insert(QStringLiteral("id"), static_cast<double>(id));
    request.insert(QStringLiteral("method"), method);
    request.insert(QStringLiteral("params"), params);
    return request;
}

QByteArray htmlPage(const QString& body) {
    const QByteArray html = "<!doctype html><html><body style=\"font-family:sans-serif\"><p>"
        + body.toUtf8() + "</p></body></html>";
    return "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\nContent-Length: "
        + QByteArray::number(html.size()) + "\r\n\r\n" + html;
}

QString extractHeaderParameter(const QByteArray& rawHeader, const QString& key) {
    const QString text = QString::fromUtf8(rawHeader);
    const QRegularExpression quoted(key + QStringLiteral(R"regex(\s*=\s*"([^"]*)")regex"),
                                    QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch match = quoted.match(text); match.hasMatch()) {
        return match.captured(1).trimmed();
    }
    const QRegularExpression unquoted(key + QStringLiteral(R"(\s*=\s*([^\s,]+))"),
                                      QRegularExpression::CaseInsensitiveOption);
    if (const QRegularExpressionMatch match = unquoted.match(text); match.hasMatch()) {
        return match.captured(1).trimmed();
    }
    return {};
}

QList<QUrl> protectedResourceMetadataCandidates(const QUrl& mcpEndpoint, const QString& challengeUrl) {
    QList<QUrl> urls;
    if (!challengeUrl.isEmpty()) {
        const QUrl explicitUrl(challengeUrl);
        if (explicitUrl.isValid() && !explicitUrl.scheme().isEmpty()) {
            urls.append(explicitUrl);
        }
    }
    QUrl base = mcpEndpoint;
    base.setQuery(QString());
    base.setFragment(QString());
    const QString path = base.path();
    if (!path.isEmpty() && path != QStringLiteral("/")) {
        QUrl pathSpecific = base;
        pathSpecific.setPath(QStringLiteral("/.well-known/oauth-protected-resource") + path);
        if (!urls.contains(pathSpecific)) {
            urls.append(pathSpecific);
        }
    }
    QUrl root = base;
    root.setPath(QStringLiteral("/.well-known/oauth-protected-resource"));
    if (!urls.contains(root)) {
        urls.append(root);
    }
    return urls;
}

QList<QUrl> authServerMetadataCandidates(const QUrl& issuerUrl) {
    QList<QUrl> urls;
    QUrl base = issuerUrl;
    base.setQuery(QString());
    base.setFragment(QString());
    QString path = base.path();
    while (path.endsWith(QLatin1Char('/')) && path.size() > 1) {
        path.chop(1);
    }
    if (!path.isEmpty() && path != QStringLiteral("/")) {
        QUrl rfc8414Path = base;
        rfc8414Path.setPath(QStringLiteral("/.well-known/oauth-authorization-server") + path);
        urls.append(rfc8414Path);

        QUrl oidcInserted = base;
        oidcInserted.setPath(QStringLiteral("/.well-known/openid-configuration") + path);
        urls.append(oidcInserted);

        QUrl oidcAppended = base;
        oidcAppended.setPath(path + QStringLiteral("/.well-known/openid-configuration"));
        urls.append(oidcAppended);
    } else {
        QUrl rfc8414Root = base;
        rfc8414Root.setPath(QStringLiteral("/.well-known/oauth-authorization-server"));
        urls.append(rfc8414Root);

        QUrl oidcRoot = base;
        oidcRoot.setPath(QStringLiteral("/.well-known/openid-configuration"));
        urls.append(oidcRoot);
    }
    return urls;
}

} // namespace

McpServerConfig McpServerConfig::undermindDefault() {
    McpServerConfig cfg;
    cfg.id = QString::fromLatin1(kAccount);
    cfg.displayName = QStringLiteral("Undermind");
    cfg.mcpUrl = QString::fromLatin1(kMcpUrl);
    cfg.resource = QString::fromLatin1(kResource);
    cfg.authorizeUrl = QString::fromLatin1(kAuthorizeUrl);
    cfg.tokenUrl = QString::fromLatin1(kTokenUrl);
    cfg.scope = QStringLiteral("mcp");
    cfg.clientId = kestrelCimdClientId();
    return cfg;
}

QString pkceChallenge(const QString& verifier) {
    const QByteArray digest = QCryptographicHash::hash(verifier.toLatin1(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

QString pkceVerifier() {
    const QString alphabet = QString::fromLatin1(kUnreserved);
    QRandomGenerator* generator = QRandomGenerator::system();
    QString verifier;
    verifier.reserve(64);
    for (int i = 0; i < 64; ++i) {
        verifier.append(alphabet.at(generator->bounded(alphabet.size())));
    }
    return verifier;
}

QString undermindClientId() {
    return QString::fromLatin1(kClientId);
}

QString kestrelCimdClientId() {
    return QString::fromLatin1(kClientId);
}

std::optional<QJsonObject> jsonRpcResponse(const QByteArray& body, const QByteArray& contentType,
                                           std::int64_t id) {
    const auto matches = [id](const QJsonObject& object) {
        const QJsonValue value = object.value(QStringLiteral("id"));
        return object.value(QStringLiteral("jsonrpc")).toString() == QStringLiteral("2.0")
            && value.isDouble() && value.toDouble() == static_cast<double>(id);
    };
    if (contentType.contains("text/event-stream")) {
        QByteArray eventData;
        for (QByteArray line : body.split('\n')) {
            if (line.endsWith('\r')) { line.chop(1); }
            if (line.isEmpty()) {
                const QJsonDocument document = QJsonDocument::fromJson(eventData);
                if (document.isObject() && matches(document.object())) {
                    return document.object();
                }
                eventData.clear();
            } else if (line.startsWith("data:")) {
                QByteArray data = line.mid(5);
                if (data.startsWith(' ')) { data.remove(0, 1); }
                if (!eventData.isEmpty()) { eventData += '\n'; }
                eventData += data;
            }
        }
        return std::nullopt;
    }
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (document.isObject() && matches(document.object())) {
        return document.object();
    }
    return std::nullopt;
}

UndermindClient::UndermindClient(QObject* parent)
    : UndermindClient(McpServerConfig::undermindDefault(), parent) {}

UndermindClient::UndermindClient(McpServerConfig config, QObject* parent)
    : QObject(parent),
      m_config(std::move(config)),
      m_network(new QNetworkAccessManager(this)),
      m_store(storage::makePlatformSecretStore()),
      m_status(QStringLiteral("Not connected")) {
    loadTokens();
}

UndermindClient::UndermindClient(QNetworkAccessManager& network,
                                 std::unique_ptr<storage::SecretStore> store, QObject* parent)
    : UndermindClient(McpServerConfig::undermindDefault(), network, std::move(store), parent) {}

UndermindClient::UndermindClient(McpServerConfig config, QNetworkAccessManager& network,
                                 std::unique_ptr<storage::SecretStore> store, QObject* parent)
    : QObject(parent),
      m_config(std::move(config)),
      m_network(&network),
      m_store(std::move(store)),
      m_status(QStringLiteral("Not connected")) {
    loadTokens();
}

UndermindClient::~UndermindClient() {
    closeListener();
}

QString UndermindClient::effectiveClientId() const {
    return m_config.clientId.trimmed().isEmpty() ? kestrelCimdClientId() : m_config.clientId.trimmed();
}

QString UndermindClient::effectiveResource() const {
    if (!m_config.resource.trimmed().isEmpty()) {
        return m_config.resource.trimmed();
    }
    QUrl canonical(m_config.mcpUrl.trimmed());
    canonical.setFragment(QString());
    return canonical.toString();
}

QString UndermindClient::keystoreAccount() const {
    return m_config.id.trimmed().isEmpty() ? QString::fromLatin1(kAccount) : m_config.id.trimmed();
}

void UndermindClient::setStatus(const QString& status) {
    if (status != m_status) {
        m_status = status;
        emit statusChanged();
    }
}

void UndermindClient::disconnectSession(const QString& reason) {
    m_connected = false;
    m_session.clear();
    m_tools = QJsonArray{};
    m_resources = QJsonArray{};
    m_prompts = QJsonArray{};
    m_serverCapabilities = QJsonObject{};
    setStatus(QStringLiteral("Not connected: ") + reason);
}

void UndermindClient::connectAndList(std::function<void(bool, QJsonArray)> done) {
    connectAndListImpl(true, std::move(done));
}

bool UndermindClient::hasSavedTokens() {
    if (!m_config.bearerToken.trimmed().isEmpty()) {
        return true;
    }
    std::string unavailable;
    if (!m_store->available(unavailable)) {
        return false;
    }
    if (m_accessToken.isEmpty() && m_refreshToken.isEmpty()) {
        loadTokens();
    }
    return !m_accessToken.isEmpty() || !m_refreshToken.isEmpty();
}

void UndermindClient::connectSavedAndList(std::function<void(bool, QJsonArray)> done) {
    connectAndListImpl(false, std::move(done));
}

void UndermindClient::connectAndListImpl(bool allowInteractiveSignIn,
                                         std::function<void(bool, QJsonArray)> done) {
    if (m_connecting) {
        done(false, QJsonArray{});
        return;
    }
    m_connecting = true;
    m_connected = false;
    m_session.clear();
    const QString serverLabel = m_config.displayName.trimmed().isEmpty()
        ? QStringLiteral("MCP server")
        : m_config.displayName.trimmed();
    setStatus(QStringLiteral("Connecting to %1...").arg(serverLabel));
    const auto fail = [this, done](const QString& reason) {
        m_connecting = false;
        disconnectSession(reason);
        done(false, QJsonArray{});
    };

    const auto authenticated = [this, done, fail, serverLabel](bool ok, QString error) {
        if (!ok) {
            fail(error);
            return;
        }
        m_session.clear();
        const std::int64_t initId = m_nextId++;
        QJsonObject clientInfo;
        clientInfo.insert(QStringLiteral("name"), QStringLiteral("Kestrel"));
        clientInfo.insert(QStringLiteral("version"), QStringLiteral("0.1"));
        QJsonObject initParams;
        initParams.insert(QStringLiteral("protocolVersion"), QString::fromLatin1(kProtocolVersion));
        initParams.insert(QStringLiteral("capabilities"), QJsonObject{});
        initParams.insert(QStringLiteral("clientInfo"), clientInfo);

        postMcp(rpcRequest(initId, QStringLiteral("initialize"), initParams),
                [this, done, fail, initId, serverLabel](HttpResult init) {
                    if (init.status == 401) {
                        forgetTokens();
                        fail(QStringLiteral("%1 rejected the saved sign-in. Click Connect to sign in again.").arg(serverLabel));
                        return;
                    }
                    const auto response = jsonRpcResponse(init.body, init.contentType, initId);
                    if (!init.error.isEmpty() || !response || response->contains(QStringLiteral("error"))
                        || !response->value(QStringLiteral("result")).isObject()
                        || response->value(QStringLiteral("result")).toObject()
                               .value(QStringLiteral("protocolVersion")).toString() != QString::fromLatin1(kProtocolVersion)) {
                        fail(init.error.isEmpty()
                                 ? QStringLiteral("%1 did not answer the handshake.").arg(serverLabel)
                                 : init.error);
                        return;
                    }
                    m_session = init.session;
                    m_serverCapabilities = response->value(QStringLiteral("result"))
                                               .toObject()
                                               .value(QStringLiteral("capabilities"))
                                               .toObject();

                    // A notification has no id and no reply body; the server answers 202.
                    QJsonObject notice;
                    notice.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
                    notice.insert(QStringLiteral("method"), QStringLiteral("notifications/initialized"));
                    postMcp(notice, [this, done, fail, serverLabel](HttpResult ack) {
                        if (ack.status == 401) {
                            forgetTokens();
                        }
                        if (!ack.error.isEmpty() || ack.status < 200 || ack.status >= 300) {
                            fail(ack.error.isEmpty()
                                     ? QStringLiteral("%1 refused the session (HTTP %2). Click Connect to retry.")
                                           .arg(serverLabel)
                                           .arg(ack.status)
                                     : ack.error);
                            return;
                        }
                        if (!m_serverCapabilities.contains(QStringLiteral("tools"))) {
                            m_tools = QJsonArray{};
                            discoverOptionalCapabilities(m_tools, done);
                            return;
                        }
                        const std::int64_t listId = m_nextId++;
                        postMcp(rpcRequest(listId, QStringLiteral("tools/list"), QJsonObject{}),
                                [this, done, fail, listId, serverLabel](HttpResult listing) {
                                    const auto response = jsonRpcResponse(listing.body, listing.contentType, listId);
                                    if (listing.status == 401) {
                                        forgetTokens();
                                    }
                                    if (!listing.error.isEmpty() || !response
                                        || response->contains(QStringLiteral("error"))
                                        || !response->value(QStringLiteral("result")).isObject()) {
                                        fail(listing.error.isEmpty()
                                                 ? QStringLiteral("%1 did not list its tools.").arg(serverLabel)
                                                 : listing.error);
                                        return;
                                    }
                                    const QJsonValue listed = response->value(QStringLiteral("result"))
                                                                  .toObject().value(QStringLiteral("tools"));
                                    if (!listed.isArray()) {
                                        fail(QStringLiteral("%1 returned an invalid tool list. Click Connect to retry.").arg(serverLabel));
                                        return;
                                    }
                                    const QJsonArray tools = listed.toArray();
                                    for (const QJsonValue& value : tools) {
                                        const QJsonObject tool = value.toObject();
                                        if (tool.value(QStringLiteral("name")).toString().trimmed().isEmpty()
                                            || !tool.value(QStringLiteral("inputSchema")).isObject()) {
                                            fail(QStringLiteral("%1 returned an invalid tool definition. Click Connect to retry.").arg(serverLabel));
                                            return;
                                        }
                                    }
                                    m_tools = tools;
                                    discoverOptionalCapabilities(tools, done);
                                });
                    });
                });
    };
    if (!m_config.bearerToken.trimmed().isEmpty()) {
        m_accessToken = m_config.bearerToken.trimmed();
        authenticated(true, QString());
        return;
    }
    if (allowInteractiveSignIn) {
        // An explicit Connect is a fresh PKCE authorization request, even
        // when startup could have resumed an existing grant silently.
        ensureOAuthEndpoints([this, authenticated](bool ok, QString error) {
            if (!ok || m_config.authorizeUrl.isEmpty()) {
                authenticated(ok, error);
                return;
            }
            startSignIn(authenticated);
        });
    } else {
        ensureToken(false, authenticated);
    }
}

void UndermindClient::discoverOptionalCapabilities(
    const QJsonArray& tools, std::function<void(bool ok, QJsonArray tools)> done) {
    const QString serverLabel = m_config.displayName.trimmed().isEmpty()
        ? QStringLiteral("MCP server")
        : m_config.displayName.trimmed();
    m_resources = QJsonArray{};
    m_prompts = QJsonArray{};

    const auto finish = [this, tools, serverLabel, done]() {
        m_connecting = false;
        m_connected = true;
        QString summary = QStringLiteral("Connected to %1, %2 tools").arg(serverLabel).arg(tools.size());
        if (!m_resources.isEmpty()) {
            summary += QStringLiteral(", %1 resources").arg(m_resources.size());
        }
        if (!m_prompts.isEmpty()) {
            summary += QStringLiteral(", %1 prompts").arg(m_prompts.size());
        }
        setStatus(summary);
        done(true, tools);
    };

    const auto discoverPrompts = [this, finish]() {
        if (!m_serverCapabilities.contains(QStringLiteral("prompts"))
            || !m_serverCapabilities.value(QStringLiteral("prompts")).isObject()) {
            finish();
            return;
        }
        const std::int64_t promptId = m_nextId++;
        postMcp(rpcRequest(promptId, QStringLiteral("prompts/list"), QJsonObject{}),
                [this, promptId, finish](HttpResult reply) {
                    if (const auto response = jsonRpcResponse(reply.body, reply.contentType, promptId);
                        reply.error.isEmpty() && response && response->value(QStringLiteral("result")).isObject()) {
                        const QJsonValue prompts = response->value(QStringLiteral("result"))
                                                       .toObject()
                                                       .value(QStringLiteral("prompts"));
                        if (prompts.isArray()) {
                            m_prompts = prompts.toArray();
                        }
                    }
                    finish();
                });
    };

    if (!m_serverCapabilities.contains(QStringLiteral("resources"))
        || !m_serverCapabilities.value(QStringLiteral("resources")).isObject()) {
        discoverPrompts();
        return;
    }
    const std::int64_t resourceId = m_nextId++;
    postMcp(rpcRequest(resourceId, QStringLiteral("resources/list"), QJsonObject{}),
            [this, resourceId, discoverPrompts](HttpResult reply) {
                if (const auto response = jsonRpcResponse(reply.body, reply.contentType, resourceId);
                    reply.error.isEmpty() && response && response->value(QStringLiteral("result")).isObject()) {
                    const QJsonValue resources = response->value(QStringLiteral("result"))
                                                     .toObject()
                                                     .value(QStringLiteral("resources"));
                    if (resources.isArray()) {
                        m_resources = resources.toArray();
                    }
                }
                discoverPrompts();
            });
}

void UndermindClient::callTool(const std::string& name, const std::string& argumentsJson,
                               core::McpCompletion done) {
    const QString serverLabel = m_config.displayName.trimmed().isEmpty()
        ? QStringLiteral("MCP server")
        : m_config.displayName.trimmed();
    if (!m_connected) {
        done(core::McpToolResult{
            true, (serverLabel + QStringLiteral(" is not connected. Click Connect to sign in and discover tools."))
                      .toStdString()});
        return;
    }
    const QJsonDocument parsed = QJsonDocument::fromJson(QByteArray::fromStdString(argumentsJson));
    QJsonObject params;
    params.insert(QStringLiteral("name"), QString::fromStdString(name));
    params.insert(QStringLiteral("arguments"), parsed.isObject() ? parsed.object() : QJsonObject{});
    const std::int64_t id = m_nextId++;

    postMcp(rpcRequest(id, QStringLiteral("tools/call"), params),
            [this, id, serverLabel, done = std::move(done)](HttpResult reply) {
                if (reply.status == 401) {
                    forgetTokens();
                    const QString message = QStringLiteral("%1 rejected the sign-in. Click Connect to sign in again.")
                                                .arg(serverLabel);
                    disconnectSession(message);
                    done(core::McpToolResult{true, message.toStdString()});
                    return;
                }
                if (reply.status == 404) {
                    // The server ended the session; the next connect starts a new one.
                    const QString message = QStringLiteral("%1's session ended. Click Connect to reconnect.")
                                                .arg(serverLabel);
                    disconnectSession(message);
                    done(core::McpToolResult{true, message.toStdString()});
                    return;
                }
                const auto response = jsonRpcResponse(reply.body, reply.contentType, id);
                if (!reply.error.isEmpty() || !response
                    || (!response->value(QStringLiteral("result")).isObject()
                        && !response->value(QStringLiteral("error")).isObject())) {
                    const QString reason = reply.error.isEmpty()
                        ? QStringLiteral("%1 did not answer. Click Connect to reconnect.").arg(serverLabel)
                        : reply.error;
                    disconnectSession(reason);
                    done(core::McpToolResult{true, reason.toStdString()});
                    return;
                }
                if (const QJsonValue failure = response->value(QStringLiteral("error")); failure.isObject()) {
                    done(core::McpToolResult{true, failure.toObject().value(QStringLiteral("message"))
                                                      .toString().toStdString()});
                    return;
                }
                const QJsonObject result = response->value(QStringLiteral("result")).toObject();
                QString text;
                for (const QJsonValue& item : result.value(QStringLiteral("content")).toArray()) {
                    const QJsonObject block = item.toObject();
                    const QString type = block.value(QStringLiteral("type")).toString();
                    if (type == QStringLiteral("text")) {
                        text += block.value(QStringLiteral("text")).toString() + QLatin1Char('\n');
                    } else if (type == QStringLiteral("resource") && block.value(QStringLiteral("resource")).isObject()) {
                        const QJsonObject res = block.value(QStringLiteral("resource")).toObject();
                        text += QStringLiteral("[Resource %1]\n%2\n")
                                    .arg(res.value(QStringLiteral("uri")).toString(),
                                         res.value(QStringLiteral("text")).toString());
                    } else if (type == QStringLiteral("resource_link")) {
                        text += QStringLiteral("[Resource link: %1 (%2)]\n")
                                    .arg(block.value(QStringLiteral("name")).toString(),
                                         block.value(QStringLiteral("uri")).toString());
                    }
                }
                if (text.trimmed().isEmpty() && result.value(QStringLiteral("structuredContent")).isObject()) {
                    text = QString::fromUtf8(
                        QJsonDocument(result.value(QStringLiteral("structuredContent")).toObject())
                            .toJson(QJsonDocument::Compact));
                }
                done(core::McpToolResult{result.value(QStringLiteral("isError")).toBool(),
                                         text.trimmed().toStdString()});
            });
}

void UndermindClient::readResource(const QString& uri, std::function<void(core::McpToolResult)> done) {
    const QString serverLabel = m_config.displayName.trimmed().isEmpty()
        ? QStringLiteral("MCP server")
        : m_config.displayName.trimmed();
    if (!m_connected) {
        done(core::McpToolResult{
            true, (serverLabel + QStringLiteral(" is not connected. Click Connect first.")).toStdString()});
        return;
    }
    QJsonObject params;
    params.insert(QStringLiteral("uri"), uri);
    const std::int64_t id = m_nextId++;
    postMcp(rpcRequest(id, QStringLiteral("resources/read"), params),
            [id, serverLabel, done = std::move(done)](HttpResult reply) {
                const auto response = jsonRpcResponse(reply.body, reply.contentType, id);
                if (!reply.error.isEmpty() || !response) {
                    done(core::McpToolResult{
                        true, (reply.error.isEmpty() ? serverLabel + QStringLiteral(" did not return the resource.")
                                                     : reply.error)
                                  .toStdString()});
                    return;
                }
                if (response->value(QStringLiteral("error")).isObject()) {
                    done(core::McpToolResult{
                        true, response->value(QStringLiteral("error")).toObject()
                                  .value(QStringLiteral("message")).toString().toStdString()});
                    return;
                }
                const QJsonArray contents = response->value(QStringLiteral("result"))
                                                .toObject()
                                                .value(QStringLiteral("contents"))
                                                .toArray();
                QString text;
                for (const QJsonValue& item : contents) {
                    const QJsonObject obj = item.toObject();
                    if (obj.contains(QStringLiteral("text"))) {
                        text += obj.value(QStringLiteral("text")).toString() + QLatin1Char('\n');
                    }
                }
                done(core::McpToolResult{false, text.trimmed().toStdString()});
            });
}

void UndermindClient::getPrompt(const QString& name, const QJsonObject& arguments,
                                std::function<void(core::McpToolResult)> done) {
    const QString serverLabel = m_config.displayName.trimmed().isEmpty()
        ? QStringLiteral("MCP server")
        : m_config.displayName.trimmed();
    if (!m_connected) {
        done(core::McpToolResult{
            true, (serverLabel + QStringLiteral(" is not connected. Click Connect first.")).toStdString()});
        return;
    }
    QJsonObject params;
    params.insert(QStringLiteral("name"), name);
    params.insert(QStringLiteral("arguments"), arguments);
    const std::int64_t id = m_nextId++;
    postMcp(rpcRequest(id, QStringLiteral("prompts/get"), params),
            [id, serverLabel, done = std::move(done)](HttpResult reply) {
                const auto response = jsonRpcResponse(reply.body, reply.contentType, id);
                if (!reply.error.isEmpty() || !response) {
                    done(core::McpToolResult{
                        true, (reply.error.isEmpty() ? serverLabel + QStringLiteral(" did not return the prompt.")
                                                     : reply.error)
                                  .toStdString()});
                    return;
                }
                if (response->value(QStringLiteral("error")).isObject()) {
                    done(core::McpToolResult{
                        true, response->value(QStringLiteral("error")).toObject()
                                  .value(QStringLiteral("message")).toString().toStdString()});
                    return;
                }
                const QJsonArray messages = response->value(QStringLiteral("result"))
                                                .toObject()
                                                .value(QStringLiteral("messages"))
                                                .toArray();
                QString text;
                for (const QJsonValue& item : messages) {
                    const QJsonObject msg = item.toObject();
                    const QString role = msg.value(QStringLiteral("role")).toString(QStringLiteral("user"));
                    const QJsonObject content = msg.value(QStringLiteral("content")).toObject();
                    text += role + QStringLiteral(": ") + content.value(QStringLiteral("text")).toString()
                        + QLatin1Char('\n');
                }
                done(core::McpToolResult{false, text.trimmed().toStdString()});
            });
}

void UndermindClient::ensureToken(bool allowInteractiveSignIn, Done done) {
    if (!m_config.bearerToken.trimmed().isEmpty()) {
        m_accessToken = m_config.bearerToken.trimmed();
        done(true, QString());
        return;
    }
    std::string unavailable;
    if (!m_store->available(unavailable)) {
        done(false, QString::fromStdString(unavailable));
        return;
    }
    if (m_accessToken.isEmpty() && m_refreshToken.isEmpty()) {
        loadTokens();
    }
    const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
    if (!m_accessToken.isEmpty() && m_expiresAtMs - now > kRefreshMarginMs) {
        done(true, QString());
        return;
    }
    const QString serverLabel = m_config.displayName.trimmed().isEmpty()
        ? QStringLiteral("Undermind")
        : m_config.displayName.trimmed();
    if (!m_refreshToken.isEmpty()) {
        ensureOAuthEndpoints([this, allowInteractiveSignIn, serverLabel, done](bool endpointsOk, QString endpointError) {
            if (!endpointsOk) {
                done(false, endpointError);
                return;
            }
            QUrlQuery form;
            form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
            form.addQueryItem(QStringLiteral("refresh_token"), m_refreshToken);
            form.addQueryItem(QStringLiteral("client_id"), effectiveClientId());
            form.addQueryItem(QStringLiteral("resource"), effectiveResource());
            requestTokens(form, [this, allowInteractiveSignIn, serverLabel, done](bool ok, QString error) {
                if (ok) {
                    done(true, QString());
                } else if (allowInteractiveSignIn) {
                    startSignIn(done);
                } else {
                    done(false, error.isEmpty()
                                    ? QStringLiteral("Saved %1 sign-in expired. Click Connect to sign in again.")
                                          .arg(serverLabel)
                                    : error);
                }
            });
        });
        return;
    }
    if (!allowInteractiveSignIn) {
        done(false, QStringLiteral("No saved %1 sign-in.").arg(serverLabel));
        return;
    }
    ensureOAuthEndpoints([this, done](bool ok, QString error) {
        if (!ok) {
            done(false, error);
            return;
        }
        startSignIn(done);
    });
}

void UndermindClient::ensureOAuthEndpoints(Done done) {
    if (!m_config.authorizeUrl.trimmed().isEmpty() && !m_config.tokenUrl.trimmed().isEmpty()) {
        done(true, QString());
        return;
    }
    const QUrl mcpEndpoint(m_config.mcpUrl.trimmed());
    if (!mcpEndpoint.isValid() || mcpEndpoint.scheme().isEmpty()) {
        done(false, QStringLiteral("Invalid MCP endpoint URL."));
        return;
    }
    // Probe the MCP server or its RFC 9728 Protected Resource Metadata document
    // to discover authorization_servers and scopes_supported.
    const std::int64_t probeId = m_nextId++;
    QJsonObject clientInfo{{QStringLiteral("name"), QStringLiteral("Kestrel")},
                           {QStringLiteral("version"), QStringLiteral("0.1")}};
    QJsonObject initParams{{QStringLiteral("protocolVersion"), QString::fromLatin1(kProtocolVersion)},
                           {QStringLiteral("capabilities"), QJsonObject{}},
                           {QStringLiteral("clientInfo"), clientInfo}};
    postMcp(rpcRequest(probeId, QStringLiteral("initialize"), initParams),
            [this, mcpEndpoint, done, probeId](HttpResult probe) {
                const auto response = jsonRpcResponse(probe.body, probe.contentType, probeId);
                if (probe.error.isEmpty() && response && response->value(QStringLiteral("result")).isObject()) {
                    // Public servers need no OAuth grant. The caller performs a
                    // fresh handshake without carrying this probe's session.
                    done(true, QString());
                    return;
                }
                const QString challengeMetadata = extractHeaderParameter(
                    probe.wwwAuthenticate, QStringLiteral("resource_metadata"));
                const QString challengeScope = extractHeaderParameter(
                    probe.wwwAuthenticate, QStringLiteral("scope"));
                if (!challengeScope.isEmpty() && m_config.scope.trimmed().isEmpty()) {
                    m_config.scope = challengeScope;
                }
                const QList<QUrl> candidates =
                    protectedResourceMetadataCandidates(mcpEndpoint, challengeMetadata);
                discoverProtectedResource(candidates, 0, done);
            });
}

void UndermindClient::discoverProtectedResource(const QList<QUrl>& candidates, int index, Done done) {
    if (index >= candidates.size()) {
        done(false, QStringLiteral("Could not discover OAuth Protected Resource Metadata (RFC 9728) for %1.")
                        .arg(m_config.mcpUrl));
        return;
    }
    getJson(candidates.at(index), [this, candidates, index, done](int status, QJsonObject meta, QString) {
        const QJsonArray servers = meta.value(QStringLiteral("authorization_servers")).toArray();
        if (status < 200 || status >= 300 || servers.isEmpty() || !servers.first().isString()) {
            discoverProtectedResource(candidates, index + 1, done);
            return;
        }
        const QString resourceValue = meta.value(QStringLiteral("resource")).toString().trimmed();
        if (resourceValue != effectiveResource()) {
            done(false, QStringLiteral("Protected Resource Metadata does not match this MCP resource. Check the server URL."));
            return;
        }
        QStringList scopes;
        for (const QJsonValue& item : meta.value(QStringLiteral("scopes_supported")).toArray()) {
            if (item.isString() && !item.toString().trimmed().isEmpty()) {
                scopes.append(item.toString().trimmed());
            }
        }
        const QUrl issuer(servers.first().toString().trimmed());
        if (!issuer.isValid() || issuer.scheme() != QStringLiteral("https") || issuer.host().isEmpty()
            || !issuer.userInfo().isEmpty() || issuer.hasFragment() || issuer.hasQuery()) {
            discoverProtectedResource(candidates, index + 1, done);
            return;
        }
        const QList<QUrl> authCandidates = authServerMetadataCandidates(issuer);
        discoverAuthServerMetadata(issuer, authCandidates, 0, scopes, done);
    });
}

void UndermindClient::discoverAuthServerMetadata(const QUrl& issuer, const QList<QUrl>& candidates,
                                                 int index, const QStringList& resourceScopes,
                                                 Done done) {
    if (index >= candidates.size()) {
        done(false, QStringLiteral("Could not discover OAuth Authorization Server Metadata (RFC 8414) for %1.")
                        .arg(issuer.toString()));
        return;
    }
    getJson(candidates.at(index),
            [this, issuer, candidates, index, resourceScopes, done](int status, QJsonObject meta, QString) {
                const QString authEndpoint = meta.value(QStringLiteral("authorization_endpoint")).toString().trimmed();
                const QString tokenEndpoint = meta.value(QStringLiteral("token_endpoint")).toString().trimmed();
                if (status < 200 || status >= 300 || authEndpoint.isEmpty() || tokenEndpoint.isEmpty()) {
                    discoverAuthServerMetadata(issuer, candidates, index + 1, resourceScopes, done);
                    return;
                }
                const auto secureEndpoint = [](const QString& value) {
                    const QUrl url(value);
                    return url.isValid() && url.scheme() == QStringLiteral("https")
                        && !url.host().isEmpty() && url.userInfo().isEmpty() && !url.hasFragment();
                };
                if (meta.value(QStringLiteral("issuer")).toString() != issuer.toString()
                    || !secureEndpoint(authEndpoint) || !secureEndpoint(tokenEndpoint)) {
                    done(false, QStringLiteral("OAuth metadata has a mismatched issuer or insecure endpoint. Contact the server administrator."));
                    return;
                }
                if (!meta.value(QStringLiteral("code_challenge_methods_supported")).toArray()
                         .contains(QJsonValue(QStringLiteral("S256")))) {
                    done(false, QStringLiteral("This authorization server does not advertise PKCE S256. Contact the server administrator."));
                    return;
                }
                if (effectiveClientId() == kestrelCimdClientId()
                    && !meta.value(QStringLiteral("client_id_metadata_document_supported")).toBool()) {
                    done(false, QStringLiteral("This authorization server does not support CIMD. Configure a pre-registered client ID for this server."));
                    return;
                }
                m_config.authorizeUrl = authEndpoint;
                m_config.tokenUrl = tokenEndpoint;
                m_discoveredIssuer = issuer.toString();
                m_requireIssuerInAuthResponse =
                    meta.value(QStringLiteral("authorization_response_iss_parameter_supported")).toBool(false);
                if (m_config.scope.trimmed().isEmpty() && !resourceScopes.isEmpty()) {
                    m_config.scope = resourceScopes.join(QLatin1Char(' '));
                }
                done(true, QString());
            });
}

void UndermindClient::startSignIn(Done done) {
    const std::uint64_t attempt = ++m_signInAttempt;
    closeListener();
    finishSignIn(false, QStringLiteral("A new sign-in replaced this request."));
    m_verifier = pkceVerifier();
    m_state = pkceVerifier().left(32);

    m_listener = new QTcpServer(this);
    if (!m_listener->listen(QHostAddress::LocalHost, kRedirectPort)) {
        const QString reason = m_listener->errorString();
        closeListener();
        done(false, QStringLiteral("port %1 is in use (%2). Close the program using it, then click Connect again.")
                         .arg(kRedirectPort).arg(reason));
        return;
    }

    const QString scope = m_config.scope.trimmed();
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("client_id"), effectiveClientId());
    query.addQueryItem(QStringLiteral("redirect_uri"), QString::fromLatin1(kRedirectUri));
    query.addQueryItem(QStringLiteral("code_challenge"), pkceChallenge(m_verifier));
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    if (!scope.isEmpty()) { query.addQueryItem(QStringLiteral("scope"), scope); }
    query.addQueryItem(QStringLiteral("resource"), effectiveResource());
    query.addQueryItem(QStringLiteral("state"), m_state);
    QUrl url(m_config.authorizeUrl.trimmed());
    url.setQuery(query);

    m_signInDone = std::move(done);
    const QString serverLabel = m_config.displayName.trimmed().isEmpty()
        ? QStringLiteral("Undermind")
        : m_config.displayName.trimmed();

    connect(m_listener, &QTcpServer::newConnection, this, [this, attempt, serverLabel] {
        if (attempt != m_signInAttempt || m_listener == nullptr) {
            return;
        }
        QTcpSocket* socket = m_listener->nextPendingConnection();
        if (socket == nullptr) {
            return;
        }
        // The listener closes after a valid callback, but this socket must
        // survive until the token exchange has a result for the browser.
        socket->setParent(this);
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket, [this, attempt, socket, serverLabel] {
            if (!socket->canReadLine()) {
                return;
            }
            // The request line is all the callback needs: method, path, and query.
            const QList<QByteArray> parts = socket->readLine().split(' ');
            const QUrl target(QStringLiteral("http://127.0.0.1:%1").arg(kRedirectPort)
                              + (parts.size() > 1 ? QString::fromUtf8(parts[1]) : QString()));
            if (parts.size() < 3 || parts[0] != "GET" || target.path() != QStringLiteral("/callback")) {
                socket->write("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                socket->disconnectFromHost();
                return;
            }
            if (attempt != m_signInAttempt || !m_signInDone) {
                socket->disconnectFromHost();
                return;
            }
            // Stop reading this socket before waiting for the token exchange:
            // the browser may still send headers, but they are not a new callback.
            socket->disconnect(socket, &QTcpSocket::readyRead, socket, nullptr);
            const QPointer<QTcpSocket> callbackSocket(socket);
            const auto answer = [callbackSocket, serverLabel](bool ok) {
                if (callbackSocket && callbackSocket->state() == QAbstractSocket::ConnectedState) {
                    callbackSocket->write(htmlPage(ok
                        ? QStringLiteral("Kestrel is signed in to %1. Return to Kestrel to check tool discovery.")
                              .arg(serverLabel)
                        : QStringLiteral("Sign-in did not complete. Return to Kestrel for the error and click Connect to try again.")));
                    callbackSocket->disconnectFromHost();
                }
            };
            const QUrlQuery returned(target);
            if (returned.queryItemValue(QStringLiteral("state"), QUrl::FullyDecoded) != m_state) {
                answer(false);
                finishSignIn(false, QStringLiteral("the sign-in did not match this request. Click Connect to try again."));
                return;
            }
            // RFC 9207 issuer validation when discovered or present in callback.
            const bool hasIss = returned.hasQueryItem(QStringLiteral("iss"));
            const QString returnedIss = returned.queryItemValue(QStringLiteral("iss"), QUrl::FullyDecoded);
            if ((m_requireIssuerInAuthResponse && !hasIss)
                || (hasIss && !m_discoveredIssuer.isEmpty() && returnedIss != m_discoveredIssuer)) {
                answer(false);
                finishSignIn(false, QStringLiteral("the authorization server issuer did not match %1. Click Connect to retry.")
                                        .arg(serverLabel));
                return;
            }
            closeListener();
            if (returned.hasQueryItem(QStringLiteral("error"))) {
                QString reason = returned.queryItemValue(QStringLiteral("error_description"), QUrl::FullyDecoded);
                if (reason.isEmpty()) {
                    reason = returned.queryItemValue(QStringLiteral("error"), QUrl::FullyDecoded);
                }
                answer(false);
                finishSignIn(false, QStringLiteral("%1 did not approve access: %2. Click Connect to retry.")
                                        .arg(serverLabel, reason));
                return;
            }
            const QString code = returned.queryItemValue(QStringLiteral("code"), QUrl::FullyDecoded);
            if (code.isEmpty()) {
                answer(false);
                finishSignIn(false, QStringLiteral("%1 returned no authorization code. Click Connect to try again.")
                                        .arg(serverLabel));
                return;
            }
            QUrlQuery form;
            form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
            form.addQueryItem(QStringLiteral("code"), code);
            form.addQueryItem(QStringLiteral("redirect_uri"), QString::fromLatin1(kRedirectUri));
            form.addQueryItem(QStringLiteral("client_id"), effectiveClientId());
            form.addQueryItem(QStringLiteral("code_verifier"), m_verifier);
            form.addQueryItem(QStringLiteral("resource"), effectiveResource());
            requestTokens(form, [this, attempt, answer](bool ok, QString error) {
                answer(ok && attempt == m_signInAttempt);
                if (attempt == m_signInAttempt) {
                    finishSignIn(ok, error);
                }
            });
        });
    });

    if (!QDesktopServices::openUrl(url)) {
        closeListener();
        finishSignIn(false, QStringLiteral("Kestrel could not open a browser. Open this address yourself: %1")
                                .arg(url.toString()));
        return;
    }
    setStatus(QStringLiteral("Waiting for sign-in in your browser..."));

    QTimer::singleShot(kSignInTimeoutMs, this, [this, attempt] {
        if (attempt == m_signInAttempt && m_signInDone) {
            closeListener();
            finishSignIn(false, QStringLiteral("the sign-in timed out. Click Connect to try again."));
        }
    });
}

void UndermindClient::finishSignIn(bool ok, const QString& error) {
    closeListener();
    Done done = std::exchange(m_signInDone, nullptr);
    if (done) {
        done(ok, error);
    }
}

void UndermindClient::closeListener() {
    if (m_listener != nullptr) {
        m_listener->close();
        m_listener->deleteLater();
        m_listener = nullptr;
    }
}

void UndermindClient::requestTokens(const QUrlQuery& form, Done done) {
    QNetworkRequest request{QUrl(m_config.tokenUrl.trimmed())};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    // QUrlQuery is URI-query encoding, not HTML form encoding: literal '+'
    // must be escaped or the OAuth server decodes it as a space.
    QByteArray encoded;
    for (const auto& item : form.queryItems(QUrl::FullyDecoded)) {
        if (!encoded.isEmpty()) { encoded += '&'; }
        encoded += QUrl::toPercentEncoding(item.first) + '=' + QUrl::toPercentEncoding(item.second);
    }
    const QString serverLabel = m_config.displayName.trimmed().isEmpty()
        ? QStringLiteral("Undermind")
        : m_config.displayName.trimmed();
    QNetworkReply* reply = m_network->post(request, encoded);
    connect(reply, &QNetworkReply::finished, this, [this, reply, serverLabel, done] {
        const QByteArray body = reply->readAll();
        const QString netError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        reply->deleteLater();

        const QJsonObject object = QJsonDocument::fromJson(body).object();
        if (status < 200 || status >= 300 || !netError.isEmpty()
            || object.contains(QStringLiteral("error"))
            || object.value(QStringLiteral("access_token")).toString().isEmpty()) {
            const QString reason = object.value(QStringLiteral("error_description")).toString();
            done(false, !reason.isEmpty() ? reason
                         : !netError.isEmpty() ? netError
                                               : QStringLiteral("%1 did not return a valid sign-in (HTTP %2). Click Connect to retry.")
                                                     .arg(serverLabel)
                                                     .arg(status));
            return;
        }
        const QString previousRefresh = m_refreshToken;
        const QString previousAccess = m_accessToken;
        const std::int64_t previousExpiry = m_expiresAtMs;
        m_accessToken = object.value(QStringLiteral("access_token")).toString();
        const QString refresh = object.value(QStringLiteral("refresh_token")).toString();
        m_refreshToken = refresh.isEmpty() ? previousRefresh : refresh;
        const double lifetimeSeconds = object.value(QStringLiteral("expires_in")).toDouble(3600.0);
        m_expiresAtMs = QDateTime::currentMSecsSinceEpoch() + static_cast<std::int64_t>(lifetimeSeconds) * 1000;

        QString saveError;
        if (!saveTokens(saveError)) {
            // A sign-in that cannot be kept safely is not kept at all.
            m_accessToken = previousAccess;
            m_refreshToken = previousRefresh;
            m_expiresAtMs = previousExpiry;
            done(false, QStringLiteral("Kestrel could not keep the sign-in in the system keystore: %1").arg(saveError));
            return;
        }
        done(true, QString());
    });
}

void UndermindClient::getJson(const QUrl& url,
                              std::function<void(int status, QJsonObject object, QString error)> done) {
    if (!url.isValid() || url.scheme() != QStringLiteral("https") || url.host().isEmpty()
        || !url.userInfo().isEmpty() || url.hasFragment()) {
        done(0, QJsonObject{}, QStringLiteral("OAuth discovery requires a secure HTTPS metadata URL."));
        return;
    }
    QNetworkRequest request{url};
    request.setRawHeader("Accept", "application/json");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(15000);
    QNetworkReply* reply = m_network->get(request);
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        const QString error = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        reply->deleteLater();
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        done(status, doc.isObject() ? doc.object() : QJsonObject{}, error);
    });
}

void UndermindClient::postMcp(const QJsonObject& body, std::function<void(HttpResult)> done) {
    QNetworkRequest request{QUrl(m_config.mcpUrl.trimmed())};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json, text/event-stream");
    request.setRawHeader("MCP-Protocol-Version", kProtocolVersion);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (!m_accessToken.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + m_accessToken).toUtf8());
    }
    if (!m_session.isEmpty()) {
        request.setRawHeader("Mcp-Session-Id", m_session);
    }
    request.setTransferTimeout(kToolTimeoutMs);

    const QString serverLabel = m_config.displayName.trimmed().isEmpty()
        ? QStringLiteral("Undermind")
        : m_config.displayName.trimmed();
    QNetworkReply* reply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, serverLabel, done = std::move(done)] {
        HttpResult result;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.body = reply->readAll();
        result.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        result.session = reply->rawHeader("Mcp-Session-Id");
        result.wwwAuthenticate = reply->rawHeader("WWW-Authenticate");
        if (result.status == 401) {
            result.error = QStringLiteral("%1 rejected the sign-in. Click Connect to sign in again.").arg(serverLabel);
        } else if (result.status == 403) {
            result.error = QStringLiteral("%1 denied access. Check your account permissions, then click Connect.")
                               .arg(serverLabel);
        } else if (result.status < 200 || result.status >= 300) {
            result.error = result.status == 0 ? reply->errorString()
                : QStringLiteral("%1 request failed (HTTP %2). Click Connect to retry.")
                      .arg(serverLabel)
                      .arg(result.status);
        } else if (reply->error() != QNetworkReply::NoError) {
            result.error = reply->errorString();
        }
        reply->deleteLater();
        done(std::move(result));
    });
}

void UndermindClient::loadTokens() {
    if (!m_config.bearerToken.trimmed().isEmpty()) {
        m_accessToken = m_config.bearerToken.trimmed();
        m_expiresAtMs = QDateTime::currentMSecsSinceEpoch() + 365LL * 24 * 3600 * 1000;
        return;
    }
    std::string error;
    const std::optional<std::vector<std::uint8_t>> sealed =
        m_store->sealed(keystoreAccount().toStdString(), kService, error);
    if (!sealed) {
        return;
    }
    const QByteArray bytes(reinterpret_cast<const char*>(sealed->data()), static_cast<qsizetype>(sealed->size()));
    const QJsonObject record = QJsonDocument::fromJson(bytes).object();
    // Tokens are issued to one client_id and one MCP resource. Ignore untagged
    // legacy records or records saved under an older client identity so a
    // refresh never mixes credentials across identities.
    if (record.value(QStringLiteral("client_id")).toString() != effectiveClientId()
        || record.value(QStringLiteral("resource")).toString() != effectiveResource()) {
        m_accessToken.clear();
        m_refreshToken.clear();
        m_expiresAtMs = 0;
        return;
    }
    m_accessToken = record.value(QStringLiteral("access_token")).toString();
    m_refreshToken = record.value(QStringLiteral("refresh_token")).toString();
    m_expiresAtMs = static_cast<std::int64_t>(record.value(QStringLiteral("expires_at_ms")).toDouble(0.0));
    if (m_config.authorizeUrl.trimmed().isEmpty()) {
        m_config.authorizeUrl = record.value(QStringLiteral("authorize_url")).toString();
    }
    if (m_config.tokenUrl.trimmed().isEmpty()) {
        m_config.tokenUrl = record.value(QStringLiteral("token_url")).toString();
    }
}

bool UndermindClient::saveTokens(QString& error) {
    QJsonObject record;
    record.insert(QStringLiteral("client_id"), effectiveClientId());
    record.insert(QStringLiteral("resource"), effectiveResource());
    record.insert(QStringLiteral("access_token"), m_accessToken);
    record.insert(QStringLiteral("refresh_token"), m_refreshToken);
    record.insert(QStringLiteral("expires_at_ms"), static_cast<double>(m_expiresAtMs));
    if (!m_config.authorizeUrl.trimmed().isEmpty()) {
        record.insert(QStringLiteral("authorize_url"), m_config.authorizeUrl.trimmed());
    }
    if (!m_config.tokenUrl.trimmed().isEmpty()) {
        record.insert(QStringLiteral("token_url"), m_config.tokenUrl.trimmed());
    }
    const QByteArray bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    const std::vector<std::uint8_t> blob(bytes.cbegin(), bytes.cend());
    std::string reason;
    if (!m_store->seal(keystoreAccount().toStdString(), kService, blob, reason)) {
        error = QString::fromStdString(reason);
        return false;
    }
    return true;
}

void UndermindClient::forgetTokens() {
    if (!m_config.bearerToken.trimmed().isEmpty()) {
        return;
    }
    m_accessToken.clear();
    m_refreshToken.clear();
    m_expiresAtMs = 0;
    m_session.clear();
    std::string reason;
    if (!m_store->forget(keystoreAccount().toStdString(), kService, reason)) {
        setStatus(QStringLiteral("Could not remove the saved %1 sign-in: ")
                      .arg(m_config.displayName.trimmed().isEmpty() ? QStringLiteral("MCP") : m_config.displayName)
                  + QString::fromStdString(reason));
    }
}

} // namespace kestrel::app
