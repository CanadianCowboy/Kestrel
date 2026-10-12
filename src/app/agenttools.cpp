#include "app/agenttools.h"

#include <memory>
#include <optional>
#include <string>

#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QScreen>
#include <QTimer>
#include <QUrlQuery>

#include "app/tavilyclient.h"
#include "core/agenttools.h"

namespace kestrel::app {

namespace {

constexpr int kSearchTimeoutMs = 20000;
constexpr int kMaxResultsCap = 10;

void finishOnce(const std::shared_ptr<bool>& finished, const std::function<void(core::McpToolResult)>& done,
                core::McpToolResult result) {
    if (*finished) {
        return;
    }
    *finished = true;
    done(std::move(result));
}

void handleSearxngReply(QNetworkReply* reply, const std::shared_ptr<bool>& finished,
                        const std::function<void(core::McpToolResult)>& done) {
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const std::string what = status > 0
            ? "HTTP " + std::to_string(status)
            : reply->errorString().toStdString();
        finishOnce(finished, done, core::McpToolResult{
            true, "Web search failed (" + what + "). Check the SearXNG instance and try again."});
        return;
    }
    std::string error;
    const std::optional<std::vector<core::WebSearchHit>> hits =
        core::parseSearxngResults(reply->readAll().toStdString(), error);
    if (!hits) {
        finishOnce(finished, done, core::McpToolResult{true, "Web search returned an unexpected reply: " + error});
        return;
    }
    finishOnce(finished, done, core::McpToolResult{false, core::formatSearchHits(*hits)});
}

} // namespace

void runWebSearch(QNetworkAccessManager& network, const QString& query, int maxResults,
                  std::function<void(core::McpToolResult)> done) {
    if (query.trimmed().isEmpty()) {
        done(core::McpToolResult{true, "Pass a non-empty \"query\"."});
        return;
    }
    const int limit = qBound(1, maxResults, kMaxResultsCap);

    // Tavily first: it is the configured provider, and it has the richer
    // endpoints (extract, map, crawl, research) that the other tools use.
    if (!tavilyApiKey().isEmpty()) {
        tavilySearch(network, query, limit, std::move(done));
        return;
    }

    const QString searxngBase = qEnvironmentVariable("KESTREL_SEARXNG_URL").trimmed();
    if (searxngBase.isEmpty()) {
        done(core::McpToolResult{true,
            "No web search provider is configured. Save a Tavily key to ~/.config/kestrel/tavily_api_key, or set KESTREL_SEARXNG_URL, then restart Kestrel."});
        return;
    }

    QUrl url(searxngBase + QStringLiteral("/search"));
    QUrlQuery params;
    params.addQueryItem(QStringLiteral("q"), query);
    params.addQueryItem(QStringLiteral("format"), QStringLiteral("json"));
    url.setQuery(params);
    QNetworkReply* reply = network.get(QNetworkRequest(url));

    auto finished = std::make_shared<bool>(false);
    auto* timer = new QTimer(reply);
    timer->setSingleShot(true);
    QObject::connect(timer, &QTimer::timeout, reply, [reply, finished, done] {
        finishOnce(finished, done, core::McpToolResult{true, "Web search timed out. Try again, or check the provider."});
        reply->abort();
    });
    timer->start(kSearchTimeoutMs);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, finished, done, timer] {
        timer->stop();
        handleSearxngReply(reply, finished, done);
    });
}

core::McpToolResult captureScreen(const QString& directory) {
    QScreen* screen = QGuiApplication::primaryScreen();
    if (screen == nullptr) {
        return core::McpToolResult{true, "No screen is available to capture."};
    }
    const QPixmap pixmap = screen->grabWindow(0);
    if (pixmap.isNull()) {
        return core::McpToolResult{true,
            "The screen could not be captured. On Wayland, the compositor may not allow screen capture for this app."};
    }
    QDir().mkpath(directory);
    const QString path = QDir(directory).filePath(
        QStringLiteral("screen-%1.png").arg(QDateTime::currentMSecsSinceEpoch()));
    if (!pixmap.save(path, "PNG")) {
        return core::McpToolResult{true, "The capture could not be saved to " + path.toStdString() + "."};
    }
    return core::McpToolResult{false,
        "Saved a screenshot to " + path.toStdString() + " (" + std::to_string(pixmap.width()) + "x" +
            std::to_string(pixmap.height()) + " pixels)."};
}

} // namespace kestrel::app
