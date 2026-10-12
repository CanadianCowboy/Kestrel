#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::core {

// A tool call the model wrote into its reply, in the form
//   <tool_call>{"name":"web_search","arguments":{"query":"..."}}</tool_call>
struct ModelToolCall {
    std::string name;
    // The arguments object re-serialised, so the dispatcher gets exactly what
    // the model wrote and nothing the parser reinterpreted.
    std::string argumentsJson;
};

// Returns the first complete tool call in `text`, or nullopt. A malformed or
// unterminated call is not a call: it is left as ordinary text rather than
// guessed at, so a model that is still writing never triggers a tool early.
[[nodiscard]] std::optional<ModelToolCall> findModelToolCall(std::string_view text);

struct WebSearchHit {
    std::string title;
    std::string url;
    std::string snippet;
};

// Parse a Tavily /search response: {"results":[{"title","url","content"}]}.
// On failure returns nullopt and sets `error` with what went wrong.
[[nodiscard]] std::optional<std::vector<WebSearchHit>> parseTavilyResults(std::string_view body,
                                                                          std::string& error);

// Parse a SearXNG /search?format=json response: {"results":[{"title","url","content"}]}.
[[nodiscard]] std::optional<std::vector<WebSearchHit>> parseSearxngResults(std::string_view body,
                                                                           std::string& error);

// A compact, numbered text rendering the model can read back as a tool result.
[[nodiscard]] std::string formatSearchHits(const std::vector<WebSearchHit>& hits);

} // namespace kestrel::core
