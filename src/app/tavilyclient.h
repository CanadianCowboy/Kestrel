#pragma once

#include <functional>

#include <QJsonObject>
#include <QString>
#include <QStringList>

#include "core/mcpdispatch.h"

class QNetworkAccessManager;

namespace kestrel::app {

// The Tavily API key. TAVILY_API_KEY wins; otherwise the key is read from
// ~/.config/kestrel/tavily_api_key (owner-only). Never logged, never echoed.
[[nodiscard]] QString tavilyApiKey();

// Each operation completes exactly once, possibly later, with a result the
// model can read. Output is capped so one tool call cannot flood its context.
void tavilySearch(QNetworkAccessManager& network, const QString& query, int maxResults,
                  std::function<void(core::McpToolResult)> done);
void tavilyExtract(QNetworkAccessManager& network, const QStringList& urls, const QString& query,
                   std::function<void(core::McpToolResult)> done);
void tavilyMap(QNetworkAccessManager& network, const QString& url, const QString& instructions,
               std::function<void(core::McpToolResult)> done);
void tavilyCrawl(QNetworkAccessManager& network, const QString& url, const QString& instructions,
                 std::function<void(core::McpToolResult)> done);
// Starts a research task and polls it until it completes, fails, or runs out
// of patience. Research takes minutes, so the wait is long but bounded.
void tavilyResearch(QNetworkAccessManager& network, const QString& input,
                    std::function<void(core::McpToolResult)> done);

} // namespace kestrel::app
