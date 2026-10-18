#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "core/mcpdispatch.h"

namespace kestrel::core {

// The largest request the MCP endpoint accepts, headers and body together. A
// tool call is a small JSON document; anything larger is more likely a mistake
// or an attempt to exhaust memory than a real request.
inline constexpr std::size_t kMcpMaxRequestBytes = 1024 * 1024;

// True once `raw` holds a complete request: headers, and a body as long as
// its Content-Length. Until then the transport keeps reading. A request whose
// headers are malformed is "complete" so that it gets a clear error rather
// than waiting forever.
[[nodiscard]] bool mcpRequestComplete(std::string_view raw);

struct McpHttpReply {
    int status = 500;
    std::string contentType;
    std::string body;
    std::string sessionId;
};

// Answers one complete HTTP request aimed at the MCP endpoint.
//
// The checks run in the order a hostile or confused client would hit them:
//   1. size, so an oversized request is refused before anything is parsed;
//   2. Host, which must name this machine. A browser page can be made to send
//      requests to 127.0.0.1, but it cannot make them carry a Host header for
//      that address with a name it controls -- this is the DNS-rebinding guard;
//   3. the bearer token, compared without leaking its length or prefix;
//   4. the path and method, then the JSON-RPC body itself.
// The reply is delivered through `done`, possibly after this call returns, when
// an asynchronous tool finishes. Refusals are delivered at once.
void answerMcpRequestAsync(std::string_view raw, std::string_view expectedToken,
                           McpDispatcher& dispatcher, std::function<void(McpHttpReply)> done);

// Synchronous form for callers that cannot wait. A request that completes later
// yields a 503 here.
[[nodiscard]] McpHttpReply answerMcpRequest(std::string_view raw,
                                            std::string_view expectedToken,
                                            McpDispatcher& dispatcher);

} // namespace kestrel::core
