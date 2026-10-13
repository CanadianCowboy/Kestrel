#pragma once

#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>

#include <functional>
#include <vector>

#include "core/mcpdispatch.h"

class QNetworkAccessManager;

namespace kestrel::app {

// Whether the bytes begin with the PDF header. A download that is an HTML error
// page or a redirect page does not, and must not be shown as a paper.
[[nodiscard]] bool looksLikePdf(const QByteArray& bytes);

// The name a downloaded paper is stored under. It depends only on the address,
// so the same paper is not downloaded twice into two files.
[[nodiscard]] QString cacheFileNameForUrl(const QUrl& url);

// A readable title from a file name or address: the last path part without its
// extension, with dashes and underscores turned into spaces.
[[nodiscard]] QString titleForSource(const QString& source);

// The papers the user has open, and the one the paper panel shows. The list is
// kept for the session. Downloads are stored in the application data folder.
class PaperLibrary final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList papers READ papers NOTIFY papersChanged)
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentChanged)
    Q_PROPERTY(QUrl currentSource READ currentSource NOTIFY currentChanged)
    Q_PROPERTY(bool open READ open WRITE setOpen NOTIFY openChanged)
    Q_PROPERTY(bool pdfAvailable READ pdfAvailable CONSTANT)

public:
    explicit PaperLibrary(QObject* parent = nullptr);

    [[nodiscard]] QVariantList papers() const;
    [[nodiscard]] int currentIndex() const { return m_current; }
    [[nodiscard]] QUrl currentSource() const;
    [[nodiscard]] bool open() const { return m_open; }
    void setOpen(bool open);
    // False when this build has no Qt PDF module, so papers cannot be rendered.
    [[nodiscard]] bool pdfAvailable() const;

    // Adds a PDF from disk and shows it. Used by the file dialog. Returns an empty
    // string on success, or the reason the file was refused.
    Q_INVOKABLE QString addLocalFile(const QUrl& file);
    // Shows a paper that is already in the list.
    Q_INVOKABLE void showPaper(int index);

    // Finds a paper in the list by its title, ignoring case. Returns -1 if none.
    [[nodiscard]] int indexOfTitle(const QString& title) const;

    // Downloads a PDF from an http or https address, adds it, and shows it. The
    // result is delivered once, with the paper's title on success.
    void openWeb(QNetworkAccessManager& network, const QString& address,
                 std::function<void(core::McpToolResult)> done);

signals:
    void papersChanged();
    void currentChanged();
    void openChanged();

private:
    struct Paper {
        QString title;
        QString source;   // a file URL, or the web address it came from
        QString filePath; // the local file the viewer reads
    };

    int addPaper(Paper paper);

    std::vector<Paper> m_papers;
    int m_current = -1;
    bool m_open = false;
};

} // namespace kestrel::app
