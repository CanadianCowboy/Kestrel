#pragma once

#include <functional>

#include <QString>

#include "core/mcpdispatch.h"

class QNetworkAccessManager;

namespace kestrel::app {

// Searches the web through whichever provider is configured. Tavily is used
// when TAVILY_API_KEY is set; otherwise a SearXNG instance at
// KESTREL_SEARXNG_URL. The key is read from the environment and never stored,
// logged, or written into a file. Completes exactly once, possibly later.
void runWebSearch(QNetworkAccessManager& network, const QString& query, int maxResults,
                  std::function<void(core::McpToolResult)> done);

// Captures the primary screen to a PNG under the app's data directory and
// reports where it was saved. Synchronous: grabbing one screen is quick.
[[nodiscard]] core::McpToolResult captureScreen(const QString& directory);

} // namespace kestrel::app
