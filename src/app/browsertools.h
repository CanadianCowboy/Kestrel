#pragma once

#include <QString>

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

} // namespace kestrel::app
