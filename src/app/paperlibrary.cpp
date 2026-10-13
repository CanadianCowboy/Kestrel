#include "app/paperlibrary.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>

#include <utility>

namespace kestrel::app {
namespace {

// A paper larger than this is refused. A research paper is rarely above a few
// tens of megabytes, and the cap keeps one address from filling the disk.
constexpr qint64 kMaxPaperBytes = 50LL * 1024 * 1024;
constexpr int kDownloadTimeoutMs = 60000;

bool isWebAddress(const QUrl& url) {
    return url.isValid() && (url.scheme() == QStringLiteral("http") || url.scheme() == QStringLiteral("https"));
}

QString paperCacheDirectory() {
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/papers");
}

} // namespace

bool looksLikePdf(const QByteArray& bytes) {
    return bytes.startsWith("%PDF-");
}

QString cacheFileNameForUrl(const QUrl& url) {
    const QByteArray digest = QCryptographicHash::hash(url.toEncoded(), QCryptographicHash::Sha256).toHex();
    return QString::fromLatin1(digest) + QStringLiteral(".pdf");
}

QString titleForSource(const QString& source) {
    const QUrl url(source);
    QString path = source;
    if (url.isLocalFile()) {
        path = url.toLocalFile();
    } else if (isWebAddress(url)) {
        path = url.path(QUrl::FullyDecoded);
    }
    QString title = QFileInfo(path).completeBaseName();
    title.replace(QLatin1Char('-'), QLatin1Char(' '));
    title.replace(QLatin1Char('_'), QLatin1Char(' '));
    title = title.trimmed();
    return title.isEmpty() ? QStringLiteral("Untitled paper") : title;
}

PaperLibrary::PaperLibrary(QObject* parent) : QObject(parent) {}

QVariantList PaperLibrary::papers() const {
    QVariantList list;
    for (const Paper& paper : m_papers) {
        QVariantMap entry;
        entry.insert(QStringLiteral("title"), paper.title);
        entry.insert(QStringLiteral("source"), paper.source);
        list.append(entry);
    }
    return list;
}

QUrl PaperLibrary::currentSource() const {
    if (m_current < 0 || m_current >= static_cast<int>(m_papers.size())) {
        return {};
    }
    return QUrl::fromLocalFile(m_papers[static_cast<std::size_t>(m_current)].filePath);
}

bool PaperLibrary::pdfAvailable() const {
#ifdef KESTREL_HAS_PDF
    return true;
#else
    return false;
#endif
}

void PaperLibrary::setOpen(bool open) {
    if (open != m_open) {
        m_open = open;
        emit openChanged();
    }
}

int PaperLibrary::addPaper(Paper paper) {
    // A paper already in the list is shown instead of listed twice.
    for (std::size_t i = 0; i < m_papers.size(); ++i) {
        if (m_papers[i].source == paper.source) {
            return static_cast<int>(i);
        }
    }
    m_papers.push_back(std::move(paper));
    emit papersChanged();
    return static_cast<int>(m_papers.size()) - 1;
}

void PaperLibrary::showPaper(int index) {
    if (index < 0 || index >= static_cast<int>(m_papers.size())) {
        return;
    }
    m_current = index;
    emit currentChanged();
    setOpen(true);
}

QString PaperLibrary::addLocalFile(const QUrl& file) {
    if (!file.isLocalFile()) {
        return QStringLiteral("Choose a file on this computer.");
    }
    const QString path = file.toLocalFile();
    QFile check(path);
    if (!check.open(QIODevice::ReadOnly) || !looksLikePdf(check.read(5))) {
        return QStringLiteral("That file is not a PDF, so it cannot be shown as a paper.");
    }
    check.close();
    showPaper(addPaper(Paper{titleForSource(path), file.toString(), path}));
    return {};
}

int PaperLibrary::indexOfTitle(const QString& title) const {
    const QString wanted = title.trimmed();
    if (wanted.isEmpty()) {
        return -1;
    }
    for (std::size_t i = 0; i < m_papers.size(); ++i) {
        if (m_papers[i].title.compare(wanted, Qt::CaseInsensitive) == 0) {
            return static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < m_papers.size(); ++i) {
        if (m_papers[i].title.contains(wanted, Qt::CaseInsensitive)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

void PaperLibrary::openWeb(QNetworkAccessManager& network, const QString& address,
                           std::function<void(core::McpToolResult)> done) {
    const QUrl url(address.trimmed());
    if (!isWebAddress(url)) {
        done(core::McpToolResult{true, "Only http and https addresses can be opened as papers."});
        return;
    }

    // A paper already listed, or already downloaded earlier, is shown without a
    // second download.
    const QString source = url.toString();
    const QString cachePath = paperCacheDirectory() + QLatin1Char('/') + cacheFileNameForUrl(url);
    if (const int existing = indexOfTitle(titleForSource(source));
        existing >= 0 && m_papers[static_cast<std::size_t>(existing)].source == source) {
        showPaper(existing);
        done(core::McpToolResult{false, "Showing \"" + m_papers[static_cast<std::size_t>(existing)].title.toStdString() + "\" in the paper panel."});
        return;
    }
    if (QFile::exists(cachePath)) {
        const QString title = titleForSource(source);
        showPaper(addPaper(Paper{title, source, cachePath}));
        done(core::McpToolResult{false, "Opened \"" + title.toStdString() + "\" in the paper panel."});
        return;
    }

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(kDownloadTimeoutMs);
    request.setRawHeader("User-Agent", "Kestrel/0.1 (+https://github.com/CanadianCowboy/Kestrel)");
    request.setRawHeader("Accept", "application/pdf");

    QNetworkReply* reply = network.get(request);
    // Stop as soon as the paper is larger than we will keep, rather than
    // downloading the rest of it first.
    QObject::connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64) {
        if (received > kMaxPaperBytes) {
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, source, cachePath, done = std::move(done)] {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString failure = reply->error() == QNetworkReply::NoError ? QString() : reply->errorString();
        QByteArray body = reply->readAll();
        reply->deleteLater();

        if (body.size() > kMaxPaperBytes) {
            done(core::McpToolResult{true, "The paper is larger than 50 MB, so Kestrel will not keep it."});
            return;
        }
        if (status == 0 && !failure.isEmpty()) {
            done(core::McpToolResult{true, "Could not download the paper: " + failure.toStdString()});
            return;
        }
        if (status >= 400) {
            done(core::McpToolResult{true, "The paper's site answered with HTTP status " + std::to_string(status) + "."});
            return;
        }
        if (!looksLikePdf(body)) {
            done(core::McpToolResult{true,
                                     "The address did not return a PDF. Use browser_open to see what it is."});
            return;
        }
        if (!QDir().mkpath(paperCacheDirectory())) {
            done(core::McpToolResult{true, "Could not create the paper folder in the app data directory."});
            return;
        }
        QFile file(cachePath);
        if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size()) {
            done(core::McpToolResult{true, "Could not save the paper in the app data directory."});
            return;
        }
        file.close();

        const QString title = titleForSource(source);
        showPaper(addPaper(Paper{title, source, cachePath}));
        done(core::McpToolResult{false, "Opened \"" + title.toStdString() + "\" in the paper panel."});
    });
}

} // namespace kestrel::app
