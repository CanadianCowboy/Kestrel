#include "app/browsertools.h"

#include <QDesktopServices>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

#include <string>
#include <string_view>

#include "core/webtext.h"

namespace kestrel::app {
namespace {

// A page larger than this is read only up to the limit: a model cannot use a
// megabyte of markup, and the limit keeps one bad address from exhausting memory.
constexpr qint64 kMaxPageBytes = 2 * 1024 * 1024;
constexpr int kPageTimeoutMs = 30000;
// The text handed to the model. It is cut with a note, so the model knows.
constexpr std::size_t kMaxTextChars = 12000;

bool isWebAddress(const QUrl& url) {
    return url.isValid() && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"));
}

} // namespace

bool openInBrowser(const QString& address, QString& error) {
    const QUrl url(address.trimmed());
    if (!isWebAddress(url)) {
        error = QStringLiteral("Only http and https addresses can be opened.");
        return false;
    }
    if (!QDesktopServices::openUrl(url)) {
        error = QStringLiteral("No browser could be started. Open this address yourself: %1").arg(url.toString());
        return false;
    }
    return true;
}

void readWebPage(QNetworkAccessManager& network, const QString& address,
                 std::function<void(core::McpToolResult)> done) {
    const QUrl url(address.trimmed());
    if (!isWebAddress(url)) {
        done(core::McpToolResult{true, "Only http and https addresses can be read."});
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kPageTimeoutMs);
    request.setRawHeader("User-Agent", "Kestrel/0.1 (+https://github.com/CanadianCowboy/Kestrel)");
    request.setRawHeader("Accept", "text/html,application/xhtml+xml,text/plain;q=0.9");

    QNetworkReply* reply = network.get(request);
    QObject::connect(reply, &QNetworkReply::finished, reply, [reply, done = std::move(done)] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString failure = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        const QString contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        QByteArray body = reply->readAll();
        reply->deleteLater();

        if (status == 0 && !failure.isEmpty()) {
            done(core::McpToolResult{true, "Could not load the page: " + failure.toStdString()});
            return;
        }
        if (status >= 400) {
            done(core::McpToolResult{true, "The page answered with HTTP status " + std::to_string(status) + "."});
            return;
        }
        if (body.size() > kMaxPageBytes) {
            body.truncate(kMaxPageBytes);
        }
        const std::string_view bytes(body.constData(), static_cast<std::size_t>(body.size()));
        const std::string text = contentType.contains(QStringLiteral("html"))
            ? core::htmlToText(bytes, kMaxTextChars)
            : std::string(bytes.substr(0, kMaxTextChars));
        if (text.empty()) {
            done(core::McpToolResult{false,
                                     "The page has no readable text. Many sites build their text with scripts, "
                                     "which this reader does not run. Use browser_open to see it in your browser."});
            return;
        }
        done(core::McpToolResult{false, text});
    });
}

} // namespace kestrel::app
