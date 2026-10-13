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
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <utility>
#include <vector>

namespace kestrel::app {
namespace {

// The client ID is the URL of the metadata document in docs/mcp, which Undermind
// fetches to learn Kestrel's name and redirect before the user approves access.
constexpr char kClientId[] =
    "https://cdn.jsdelivr.net/gh/CanadianCowboy/Kestrel@feature/cross-os-persona/docs/mcp/kestrel-client.json";
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

} // namespace

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

std::optional<QJsonObject> jsonRpcResponse(const QByteArray& body, const QByteArray& contentType,
                                           std::int64_t id) {
    const auto matches = [id](const QJsonObject& object) {
        const QJsonValue value = object.value(QStringLiteral("id"));
        return value.isDouble() && static_cast<std::int64_t>(value.toDouble()) == id;
    };
    if (contentType.contains("text/event-stream")) {
        for (const QByteArray& rawLine : body.split('\n')) {
            const QByteArray line = rawLine.trimmed();
            if (!line.startsWith("data:")) {
                continue;
            }
            const QJsonDocument document = QJsonDocument::fromJson(line.mid(5).trimmed());
            if (document.isObject() && matches(document.object())) {
                return document.object();
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
    : QObject(parent),
      m_network(new QNetworkAccessManager(this)),
      m_store(storage::makePlatformSecretStore()),
      m_status(QStringLiteral("Not connected")) {
    loadTokens();
}

UndermindClient::~UndermindClient() {
    closeListener();
}

void UndermindClient::setStatus(const QString& status) {
    if (status != m_status) {
        m_status = status;
        emit statusChanged();
    }
}

void UndermindClient::connectAndList(std::function<void(bool, QJsonArray)> done) {
    setStatus(QStringLiteral("Connecting to Undermind..."));
    const auto fail = [this, done](const QString& reason) {
        setStatus(QStringLiteral("Not connected: ") + reason);
        done(false, QJsonArray{});
    };

    ensureToken([this, done, fail](bool ok, QString error) {
        if (!ok) {
            fail(error);
            return;
        }
        const std::int64_t initId = m_nextId++;
        QJsonObject clientInfo;
        clientInfo.insert(QStringLiteral("name"), QStringLiteral("Kestrel"));
        clientInfo.insert(QStringLiteral("version"), QStringLiteral("0.1"));
        QJsonObject initParams;
        initParams.insert(QStringLiteral("protocolVersion"), QString::fromLatin1(kProtocolVersion));
        initParams.insert(QStringLiteral("capabilities"), QJsonObject{});
        initParams.insert(QStringLiteral("clientInfo"), clientInfo);

        postMcp(rpcRequest(initId, QStringLiteral("initialize"), initParams),
                [this, done, fail, initId](HttpResult init) {
                    if (init.status == 401) {
                        forgetTokens();
                        fail(QStringLiteral("Undermind rejected the saved sign-in. Click Connect to sign in again."));
                        return;
                    }
                    const auto response = jsonRpcResponse(init.body, init.contentType, initId);
                    if (!init.error.isEmpty() || !response || !response->contains(QStringLiteral("result"))) {
                        fail(init.error.isEmpty() ? QStringLiteral("Undermind did not answer the handshake.") : init.error);
                        return;
                    }
                    m_session = init.session;

                    // A notification has no id and no reply body; the server answers 202.
                    QJsonObject notice;
                    notice.insert(QStringLiteral("jsonrpc"), QStringLiteral("2.0"));
                    notice.insert(QStringLiteral("method"), QStringLiteral("notifications/initialized"));
                    postMcp(notice, [this, done, fail](HttpResult ack) {
                        if (ack.status >= 400) {
                            fail(QStringLiteral("Undermind refused the session (HTTP %1).").arg(ack.status));
                            return;
                        }
                        const std::int64_t listId = m_nextId++;
                        postMcp(rpcRequest(listId, QStringLiteral("tools/list"), QJsonObject{}),
                                [this, done, fail, listId](HttpResult listing) {
                                    const auto response = jsonRpcResponse(listing.body, listing.contentType, listId);
                                    if (!listing.error.isEmpty() || !response
                                        || !response->value(QStringLiteral("result")).isObject()) {
                                        fail(listing.error.isEmpty()
                                                 ? QStringLiteral("Undermind did not list its tools.")
                                                 : listing.error);
                                        return;
                                    }
                                    const QJsonArray tools = response->value(QStringLiteral("result"))
                                                                 .toObject()
                                                                 .value(QStringLiteral("tools"))
                                                                 .toArray();
                                    setStatus(QStringLiteral("Connected to Undermind, %1 tools").arg(tools.size()));
                                    done(true, tools);
                                });
                    });
                });
    });
}

void UndermindClient::callTool(const std::string& name, const std::string& argumentsJson,
                               core::McpCompletion done) {
    const QJsonDocument parsed = QJsonDocument::fromJson(QByteArray::fromStdString(argumentsJson));
    QJsonObject params;
    params.insert(QStringLiteral("name"), QString::fromStdString(name));
    params.insert(QStringLiteral("arguments"), parsed.isObject() ? parsed.object() : QJsonObject{});
    const std::int64_t id = m_nextId++;

    postMcp(rpcRequest(id, QStringLiteral("tools/call"), params),
            [this, id, done = std::move(done)](HttpResult reply) {
                if (reply.status == 401) {
                    forgetTokens();
                    setStatus(QStringLiteral("Not connected: sign in again"));
                    done(core::McpToolResult{true, "Undermind rejected the sign-in. Click Connect to sign in again."});
                    return;
                }
                if (reply.status == 404) {
                    // The server ended the session; the next connect starts a new one.
                    m_session.clear();
                    done(core::McpToolResult{true, "Undermind's session ended. Click Connect to reconnect."});
                    return;
                }
                const auto response = jsonRpcResponse(reply.body, reply.contentType, id);
                if (!response) {
                    const std::string text = reply.error.isEmpty() ? "Undermind did not answer."
                                                                   : reply.error.toStdString();
                    done(core::McpToolResult{true, text});
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
                    if (block.value(QStringLiteral("type")).toString() == QStringLiteral("text")) {
                        text += block.value(QStringLiteral("text")).toString() + QLatin1Char('\n');
                    }
                }
                done(core::McpToolResult{result.value(QStringLiteral("isError")).toBool(),
                                         text.trimmed().toStdString()});
            });
}

void UndermindClient::ensureToken(Done done) {
    std::string unavailable;
    if (!m_store->available(unavailable)) {
        done(false, QString::fromStdString(unavailable));
        return;
    }
    if (m_accessToken.isEmpty()) {
        loadTokens();
    }
    const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
    if (!m_accessToken.isEmpty() && m_expiresAtMs - now > kRefreshMarginMs) {
        done(true, QString());
        return;
    }
    if (!m_refreshToken.isEmpty()) {
        QUrlQuery form;
        form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("refresh_token"));
        form.addQueryItem(QStringLiteral("refresh_token"), m_refreshToken);
        form.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(kClientId));
        form.addQueryItem(QStringLiteral("resource"), QString::fromLatin1(kResource));
        requestTokens(form, [this, done](bool ok, QString) {
            if (ok) {
                done(true, QString());
            } else {
                startSignIn(done);
            }
        });
        return;
    }
    startSignIn(done);
}

void UndermindClient::startSignIn(Done done) {
    closeListener();
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

    QUrlQuery query;
    query.addQueryItem(QStringLiteral("response_type"), QStringLiteral("code"));
    query.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(kClientId));
    query.addQueryItem(QStringLiteral("redirect_uri"), QString::fromLatin1(kRedirectUri));
    query.addQueryItem(QStringLiteral("code_challenge"), pkceChallenge(m_verifier));
    query.addQueryItem(QStringLiteral("code_challenge_method"), QStringLiteral("S256"));
    query.addQueryItem(QStringLiteral("scope"), QStringLiteral("mcp"));
    query.addQueryItem(QStringLiteral("resource"), QString::fromLatin1(kResource));
    query.addQueryItem(QStringLiteral("state"), m_state);
    QUrl url(QString::fromLatin1(kAuthorizeUrl));
    url.setQuery(query);

    const std::uint64_t attempt = ++m_signInAttempt;
    m_signInDone = std::move(done);

    connect(m_listener, &QTcpServer::newConnection, this, [this, attempt] {
        if (attempt != m_signInAttempt || m_listener == nullptr) {
            return;
        }
        QTcpSocket* socket = m_listener->nextPendingConnection();
        if (socket == nullptr) {
            return;
        }
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket, [this, attempt, socket] {
            if (!socket->canReadLine()) {
                return;
            }
            // The request line is all the callback needs: method, path, and query.
            const QList<QByteArray> parts = socket->readLine().split(' ');
            const QUrl target(QStringLiteral("http://127.0.0.1:%1").arg(kRedirectPort)
                              + (parts.size() > 1 ? QString::fromUtf8(parts[1]) : QString()));
            if (parts.size() < 2 || target.path() != QStringLiteral("/callback")) {
                socket->write("HTTP/1.1 404 Not Found\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
                socket->disconnectFromHost();
                return;
            }
            const QUrlQuery returned(target);
            socket->write(htmlPage(QStringLiteral("Kestrel is signed in to Undermind. You can close this tab.")));
            socket->disconnectFromHost();
            if (attempt != m_signInAttempt) {
                return;
            }
            closeListener();

            if (returned.hasQueryItem(QStringLiteral("error"))) {
                finishSignIn(false, QStringLiteral("Undermind did not approve access: %1")
                                        .arg(returned.queryItemValue(QStringLiteral("error_description"))));
                return;
            }
            if (returned.queryItemValue(QStringLiteral("state")) != m_state) {
                finishSignIn(false, QStringLiteral("the sign-in did not match this request. Click Connect to try again."));
                return;
            }
            QUrlQuery form;
            form.addQueryItem(QStringLiteral("grant_type"), QStringLiteral("authorization_code"));
            form.addQueryItem(QStringLiteral("code"), returned.queryItemValue(QStringLiteral("code")));
            form.addQueryItem(QStringLiteral("redirect_uri"), QString::fromLatin1(kRedirectUri));
            form.addQueryItem(QStringLiteral("client_id"), QString::fromLatin1(kClientId));
            form.addQueryItem(QStringLiteral("code_verifier"), m_verifier);
            form.addQueryItem(QStringLiteral("resource"), QString::fromLatin1(kResource));
            requestTokens(form, [this](bool ok, QString error) { finishSignIn(ok, error); });
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
    QNetworkRequest request{QUrl(QString::fromLatin1(kTokenUrl))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    QNetworkReply* reply = m_network->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    connect(reply, &QNetworkReply::finished, this, [this, reply, done] {
        const QByteArray body = reply->readAll();
        const QString netError = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        reply->deleteLater();

        const QJsonObject object = QJsonDocument::fromJson(body).object();
        if (!object.contains(QStringLiteral("access_token"))) {
            const QString reason = object.value(QStringLiteral("error_description")).toString();
            done(false, !reason.isEmpty() ? reason
                         : !netError.isEmpty() ? netError
                                               : QStringLiteral("Undermind did not return a sign-in."));
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

void UndermindClient::postMcp(const QJsonObject& body, std::function<void(HttpResult)> done) {
    QNetworkRequest request{QUrl(QString::fromLatin1(kMcpUrl))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Accept", "application/json, text/event-stream");
    request.setRawHeader("MCP-Protocol-Version", kProtocolVersion);
    if (!m_accessToken.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + m_accessToken).toUtf8());
    }
    if (!m_session.isEmpty()) {
        request.setRawHeader("Mcp-Session-Id", m_session);
    }
    request.setTransferTimeout(kToolTimeoutMs);

    QNetworkReply* reply = m_network->post(request, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        HttpResult result;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.body = reply->readAll();
        result.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toByteArray();
        result.session = reply->rawHeader("Mcp-Session-Id");
        if (reply->error() != QNetworkReply::NoError && result.status == 0) {
            result.error = reply->errorString();
        }
        reply->deleteLater();
        done(std::move(result));
    });
}

void UndermindClient::loadTokens() {
    std::string error;
    const std::optional<std::vector<std::uint8_t>> sealed = m_store->sealed(kAccount, kService, error);
    if (!sealed) {
        return;
    }
    const QByteArray bytes(reinterpret_cast<const char*>(sealed->data()), static_cast<qsizetype>(sealed->size()));
    const QJsonObject record = QJsonDocument::fromJson(bytes).object();
    m_accessToken = record.value(QStringLiteral("access_token")).toString();
    m_refreshToken = record.value(QStringLiteral("refresh_token")).toString();
    m_expiresAtMs = static_cast<std::int64_t>(record.value(QStringLiteral("expires_at_ms")).toDouble(0.0));
}

bool UndermindClient::saveTokens(QString& error) {
    QJsonObject record;
    record.insert(QStringLiteral("access_token"), m_accessToken);
    record.insert(QStringLiteral("refresh_token"), m_refreshToken);
    record.insert(QStringLiteral("expires_at_ms"), static_cast<double>(m_expiresAtMs));
    const QByteArray bytes = QJsonDocument(record).toJson(QJsonDocument::Compact);
    const std::vector<std::uint8_t> blob(bytes.cbegin(), bytes.cend());
    std::string reason;
    if (!m_store->seal(kAccount, kService, blob, reason)) {
        error = QString::fromStdString(reason);
        return false;
    }
    return true;
}

void UndermindClient::forgetTokens() {
    m_accessToken.clear();
    m_refreshToken.clear();
    m_expiresAtMs = 0;
    m_session.clear();
    std::string reason;
    if (!m_store->forget(kAccount, kService, reason)) {
        setStatus(QStringLiteral("Could not remove the saved Undermind sign-in: ") + QString::fromStdString(reason));
    }
}

} // namespace kestrel::app
