#pragma once

#include <QObject>
#include <QString>
#include <QUrl>

#include <functional>

#include "core/mcpdispatch.h"

class QNetworkAccessManager;

namespace kestrel::app {

// Opens an http or https address in the user's default browser, which is
// Firefox on a machine where Firefox is the default. Returns false with a reason
// when the address is not allowed or no browser could be started.
[[nodiscard]] bool openInBrowser(const QString& address, QString& error);

// Fetches a page and reports its readable text. Scripts do not run, so a page
// that builds its text in JavaScript comes back empty, and the result says so.
void readWebPage(QNetworkAccessManager& network, const QString& address,
                 std::function<void(core::McpToolResult)> done);

// The embedded browser the QML side shows. Pages are shown here when Qt
// WebEngine is built in, and the agent's browser_open falls back to the user's
// default browser otherwise.
class BrowserPanel final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QUrl url READ url NOTIFY urlChanged)
    Q_PROPERTY(bool open READ open WRITE setOpen NOTIFY openChanged)
    Q_PROPERTY(bool webEngineAvailable READ webEngineAvailable CONSTANT)

public:
    explicit BrowserPanel(QObject* parent = nullptr);

    [[nodiscard]] QUrl url() const { return m_url; }
    [[nodiscard]] bool open() const { return m_open; }
    void setOpen(bool open);
    [[nodiscard]] bool webEngineAvailable() const;

    // Shows an http or https page in the panel and opens the panel.
    [[nodiscard]] bool showPage(const QString& address, QString& error);

signals:
    void urlChanged();
    void openChanged();

private:
    QUrl m_url;
    bool m_open = false;
};

} // namespace kestrel::app
