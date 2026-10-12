#include "app/tavilyclient.h"

#include <memory>

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>

namespace kestrel::app {

namespace {

constexpr auto kApiBase = "https://api.tavily.com";
constexpr int kRequestTimeoutMs = 60000;
constexpr int kResearchPollMs = 5000;
constexpr int kResearchMaxPolls = 48; // about four minutes
constexpr int kOutputCap = 12000;
constexpr int kPerSourceCap = 4000;

QString capText(const QString& text, int limit) {
    if (text.size() <= limit) {
        return text;
    }
    return text.left(limit) + QStringLiteral("\n[truncated]");
}

core::McpToolResult fail(const QString& message) {
    return core::McpToolResult{true, message.toStdString()};
}

core::McpToolResult succeed(const QString& text) {
    return core::McpToolResult{false, capText(text, kOutputCap).toStdString()};
}

using Completion = std::function<void(bool ok, QJsonObject body, QString error)>;

// One authenticated call to the Tavily API. Completes once: with the reply, or
// with a timeout. The key is sent only in the Authorization header and never
// appears in the error text.
void call(QNetworkAccessManager& network, const QByteArray& verb, const QString& path,
          const QJsonObject* body, int timeoutMs, Completion done) {
    const QString key = tavilyApiKey();
    if (key.isEmpty()) {
        done(false, {}, QStringLiteral("No Tavily key is set. Set TAVILY_API_KEY, or save the key to ~/.config/kestrel/tavily_api_key."));
        return;
    }
    QNetworkRequest request(QUrl(QString::fromLatin1(kApiBase) + path));
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", ("Bearer " + key).toUtf8());

    QNetworkReply* reply = nullptr;
    const QByteArray payload = body != nullptr ? QJsonDocument(*body).toJson(QJsonDocument::Compact) : QByteArray();
    if (verb == "POST") {
        reply = network.post(request, payload);
    } else {
        reply = network.get(request);
    }

    auto finished = std::make_shared<bool>(false);
    auto finish = [finished, done](bool ok, QJsonObject object, QString error) {
        if (*finished) {
            return;
        }
        *finished = true;
        done(ok, std::move(object), std::move(error));
    };

    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, reply, [reply, finish] {
        finish(false, {}, QStringLiteral("Tavily did not answer in time. Try again."));
        reply->abort();
    });
    timer->start(timeoutMs);

    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, finish, timer] {
        timer->stop();
        reply->deleteLater();
        const QByteArray bytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            // Show the provider's status, never the request or the key.
            finish(false, {}, status > 0
                       ? QStringLiteral("Tavily returned HTTP %1. Check the key and the request.").arg(status)
                       : QStringLiteral("Tavily could not be reached: %1").arg(reply->errorString()));
            return;
        }
        const QJsonDocument document = QJsonDocument::fromJson(bytes);
        if (!document.isObject()) {
            finish(false, {}, QStringLiteral("Tavily returned a reply Kestrel could not read."));
            return;
        }
        finish(true, document.object(), {});
    });
}

QString field(const QJsonObject& object, const char* key) {
    return object.value(QLatin1String(key)).toString();
}

void formatSources(const QJsonArray& results, QString& out) {
    int number = 1;
    for (const QJsonValue& value : results) {
        const QJsonObject item = value.toObject();
        const QString url = field(item, "url");
        if (url.isEmpty()) {
            continue;
        }
        out += QStringLiteral("%1. %2\n   %3\n").arg(number++).arg(field(item, "title"), url);
        const QString content = field(item, "content");
        if (!content.isEmpty()) {
            out += QStringLiteral("   %1\n").arg(capText(content, 400));
        }
    }
}

} // namespace

QString tavilyApiKey() {
    const QString fromEnvironment = qEnvironmentVariable("TAVILY_API_KEY").trimmed();
    if (!fromEnvironment.isEmpty()) {
        return fromEnvironment;
    }
    const QString path = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation)
        + QStringLiteral("/kestrel/tavily_api_key");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll()).trimmed();
}

void tavilySearch(QNetworkAccessManager& network, const QString& query, int maxResults,
                  std::function<void(core::McpToolResult)> done) {
    QJsonObject body;
    body.insert(QStringLiteral("query"), query);
    body.insert(QStringLiteral("max_results"), qBound(1, maxResults, 10));
    call(network, "POST", QStringLiteral("/search"), &body, kRequestTimeoutMs,
         [done](bool ok, QJsonObject reply, QString error) {
             if (!ok) {
                 done(fail(error));
                 return;
             }
             QString out;
             formatSources(reply.value(QStringLiteral("results")).toArray(), out);
             done(succeed(out.isEmpty() ? QStringLiteral("No web results.") : out));
         });
}

void tavilyExtract(QNetworkAccessManager& network, const QStringList& urls, const QString& query,
                   std::function<void(core::McpToolResult)> done) {
    if (urls.isEmpty()) {
        done(fail(QStringLiteral("Pass at least one URL in \"urls\".")));
        return;
    }
    QJsonObject body;
    QJsonArray list;
    for (const QString& url : urls.mid(0, 20)) {
        list.append(url);
    }
    body.insert(QStringLiteral("urls"), list);
    if (!query.isEmpty()) {
        body.insert(QStringLiteral("query"), query);
    }
    call(network, "POST", QStringLiteral("/extract"), &body, kRequestTimeoutMs,
         [done](bool ok, QJsonObject reply, QString error) {
             if (!ok) {
                 done(fail(error));
                 return;
             }
             QString out;
             for (const QJsonValue& value : reply.value(QStringLiteral("results")).toArray()) {
                 const QJsonObject item = value.toObject();
                 out += QStringLiteral("%1\n%2\n\n").arg(field(item, "url"),
                                                         capText(field(item, "raw_content"), kPerSourceCap));
             }
             for (const QJsonValue& value : reply.value(QStringLiteral("failed_results")).toArray()) {
                 const QJsonObject item = value.toObject();
                 out += QStringLiteral("Could not extract %1.\n").arg(field(item, "url"));
             }
             done(succeed(out.isEmpty() ? QStringLiteral("Nothing could be extracted from those URLs.") : out));
         });
}

void tavilyMap(QNetworkAccessManager& network, const QString& url, const QString& instructions,
               std::function<void(core::McpToolResult)> done) {
    QJsonObject body;
    body.insert(QStringLiteral("url"), url);
    if (!instructions.isEmpty()) {
        body.insert(QStringLiteral("instructions"), instructions);
    }
    body.insert(QStringLiteral("limit"), 50);
    call(network, "POST", QStringLiteral("/map"), &body, kRequestTimeoutMs,
         [done](bool ok, QJsonObject reply, QString error) {
             if (!ok) {
                 done(fail(error));
                 return;
             }
             QString out;
             for (const QJsonValue& value : reply.value(QStringLiteral("results")).toArray()) {
                 const QString entry = value.isString() ? value.toString() : field(value.toObject(), "url");
                 if (!entry.isEmpty()) {
                     out += entry + QLatin1Char('\n');
                 }
             }
             done(succeed(out.isEmpty() ? QStringLiteral("No pages were mapped.") : out));
         });
}

void tavilyCrawl(QNetworkAccessManager& network, const QString& url, const QString& instructions,
                 std::function<void(core::McpToolResult)> done) {
    QJsonObject body;
    body.insert(QStringLiteral("url"), url);
    if (!instructions.isEmpty()) {
        body.insert(QStringLiteral("instructions"), instructions);
    }
    body.insert(QStringLiteral("limit"), 20);
    body.insert(QStringLiteral("extract_depth"), QStringLiteral("basic"));
    call(network, "POST", QStringLiteral("/crawl"), &body, kRequestTimeoutMs,
         [done](bool ok, QJsonObject reply, QString error) {
             if (!ok) {
                 done(fail(error));
                 return;
             }
             QString out;
             for (const QJsonValue& value : reply.value(QStringLiteral("results")).toArray()) {
                 const QJsonObject item = value.toObject();
                 out += QStringLiteral("%1\n%2\n\n").arg(field(item, "url"),
                                                         capText(field(item, "raw_content"), kPerSourceCap));
             }
             done(succeed(out.isEmpty() ? QStringLiteral("The crawl found no pages.") : out));
         });
}

void tavilyResearch(QNetworkAccessManager& network, const QString& input,
                    std::function<void(core::McpToolResult)> done) {
    QJsonObject body;
    body.insert(QStringLiteral("input"), input);
    body.insert(QStringLiteral("model"), QStringLiteral("auto"));
    body.insert(QStringLiteral("stream"), false);
    call(network, "POST", QStringLiteral("/research"), &body, kRequestTimeoutMs,
         [&network, done](bool ok, QJsonObject started, QString error) {
             if (!ok) {
                 done(fail(error));
                 return;
             }
             const QString requestId = field(started, "request_id");
             if (requestId.isEmpty()) {
                 done(fail(QStringLiteral("Tavily started research but gave no request id.")));
                 return;
             }
             // Poll on a timer rather than blocking: the window stays responsive,
             // and the wait stops after a fixed number of tries.
             auto polls = std::make_shared<int>(0);
             auto poll = std::make_shared<std::function<void()>>();
             // Weak, so the poller does not keep itself alive after it stops.
             const std::weak_ptr<std::function<void()>> weakPoll = poll;
             *poll = [&network, requestId, polls, weakPoll, done] {
                 ++*polls;
                 call(network, "GET", QStringLiteral("/research/") + requestId, nullptr, kRequestTimeoutMs,
                      [&network, requestId, polls, weakPoll, done](bool ok2, QJsonObject status, QString error2) {
                          if (!ok2) {
                              done(fail(error2));
                              return;
                          }
                          const QString state = field(status, "status");
                          if (state == QStringLiteral("completed")) {
                              QString out = field(status, "content") + QStringLiteral("\n\nSources:\n");
                              formatSources(status.value(QStringLiteral("sources")).toArray(), out);
                              done(succeed(out));
                              return;
                          }
                          if (state == QStringLiteral("failed")) {
                              done(fail(QStringLiteral("Tavily research failed.")));
                              return;
                          }
                          if (*polls >= kResearchMaxPolls) {
                              done(fail(QStringLiteral("Research is still running after several minutes. Ask again later with the same request id: %1.").arg(requestId)));
                              return;
                          }
                          QTimer::singleShot(kResearchPollMs, &network, [weakPoll] {
                              if (const auto next = weakPoll.lock()) {
                                  (*next)();
                              }
                          });
                      });
             };
             QTimer::singleShot(kResearchPollMs, &network, [poll] { (*poll)(); });
             // The first poll is held by this timer; the chain then carries it on.
         });
}

} // namespace kestrel::app
