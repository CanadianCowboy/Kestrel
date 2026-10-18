#include "core/mcphttp.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace kestrel::core {

namespace {

constexpr std::string_view kEndpointPath = "/mcp";
constexpr std::string_view kHeaderEnd = "\r\n\r\n";

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
        text.remove_suffix(1);
    }
    return text;
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

struct ParsedHead {
    std::string method;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers; // names lowercased
    std::string_view body;
};

// Splits the request into its request line, headers, and the bytes after the
// blank line. Returns nullopt when the head is not yet complete.
bool validHeaderName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    for (const unsigned char c : name) {
        const bool alphaNumeric = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
            || (c >= '0' && c <= '9');
        constexpr std::string_view punctuation = "!#$%&'*+-.^_`|~";
        if (!alphaNumeric && punctuation.find(static_cast<char>(c)) == std::string_view::npos) {
            return false;
        }
    }
    return true;
}

std::optional<ParsedHead> parseHead(std::string_view raw, bool& malformed) {
    malformed = false;
    const std::size_t end = raw.find(kHeaderEnd);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view head = raw.substr(0, end);
    ParsedHead parsed;
    parsed.body = raw.substr(end + kHeaderEnd.size());

    const std::size_t firstBreak = head.find("\r\n");
    const std::string_view requestLine = head.substr(0, firstBreak);
    const std::size_t firstSpace = requestLine.find(' ');
    const std::size_t secondSpace = firstSpace == std::string_view::npos
                                        ? std::string_view::npos
                                        : requestLine.find(' ', firstSpace + 1);
    if (firstSpace == std::string_view::npos || secondSpace == std::string_view::npos) {
        malformed = true;
        return parsed;
    }
    if (requestLine.substr(secondSpace + 1) != "HTTP/1.1"
        || firstSpace == 0 || secondSpace == firstSpace + 1) {
        malformed = true;
        return parsed;
    }
    parsed.method = std::string(requestLine.substr(0, firstSpace));
    parsed.path = std::string(requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1));
    if (parsed.path.find(' ') != std::string::npos) {
        malformed = true;
        return parsed;
    }

    std::string_view rest = firstBreak == std::string_view::npos ? std::string_view{}
                                                                 : head.substr(firstBreak + 2);
    while (!rest.empty()) {
        const std::size_t lineEnd = rest.find("\r\n");
        const std::string_view line = rest.substr(0, lineEnd);
        const std::size_t colon = line.find(':');
        if (colon == std::string_view::npos) {
            malformed = true;
            return parsed;
        }
        const std::string_view name = line.substr(0, colon);
        if (!validHeaderName(name)) {
            malformed = true;
            return parsed;
        }
        const std::string normalizedName = lowercase(name);
        constexpr std::string_view securitySensitiveHeaders[] = {
            "host", "authorization", "content-length", "transfer-encoding", "origin",
            "mcp-protocol-version"};
        const bool securitySensitive = std::find(std::begin(securitySensitiveHeaders),
                                                  std::end(securitySensitiveHeaders),
                                                  normalizedName) != std::end(securitySensitiveHeaders);
        if (securitySensitive
            && std::any_of(parsed.headers.begin(), parsed.headers.end(), [&normalizedName](const auto& item) {
                   return item.first == normalizedName;
               })) {
            malformed = true;
            return parsed;
        }
        parsed.headers.emplace_back(normalizedName, std::string(trim(line.substr(colon + 1))));
        if (lineEnd == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(lineEnd + 2);
    }
    return parsed;
}

const std::string* header(const ParsedHead& head, std::string_view name) {
    for (const auto& [key, value] : head.headers) {
        if (key == name) {
            return &value;
        }
    }
    return nullptr;
}

// Compares two secrets without returning early on the first differing byte.
// The length is not hidden: a token's length is fixed and not itself secret.
bool constantTimeEquals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    unsigned char difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    }
    return difference == 0;
}

// Host must be this machine under one of the names a local client would use.
// The port is ignored, since the server may bind any port.
bool validPort(std::string_view port) {
    if (port.empty()) {
        return false;
    }
    unsigned int value = 0;
    const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), value);
    return error == std::errc{} && end == port.data() + port.size() && value > 0 && value <= 65535;
}

bool hostIsLocal(std::string_view host) {
    std::string_view name;
    std::string_view port;
    if (host.starts_with('[')) {
        const std::size_t close = host.find(']');
        if (close == std::string_view::npos || host.substr(0, close + 1) != "[::1]") {
            return false;
        }
        name = host.substr(0, close + 1);
        const std::string_view suffix = host.substr(close + 1);
        if (!suffix.empty()) {
            if (!suffix.starts_with(':')) {
                return false;
            }
            port = suffix.substr(1);
        }
    } else {
        const std::size_t colon = host.find(':');
        if (colon != std::string_view::npos) {
            if (host.find(':', colon + 1) != std::string_view::npos) {
                return false;
            }
            name = host.substr(0, colon);
            port = host.substr(colon + 1);
        } else {
            name = host;
        }
    }
    const std::string normalizedName = lowercase(name);
    const bool local = normalizedName == "[::1]" || normalizedName == "127.0.0.1"
        || normalizedName == "localhost";
    return local && (port.empty() ? host.back() != ':' : validPort(port));
}

// Per MCP Streamable HTTP security requirements: if an Origin header is
// present, it must refer to a local origin on this machine.
bool originIsLocal(std::string_view origin) {
    if (origin == "null") {
        return false;
    }
    constexpr std::string_view schemes[] = {"http://", "https://"};
    for (const std::string_view scheme : schemes) {
        if (origin.starts_with(scheme)) {
            const std::string_view authority = origin.substr(scheme.size());
            const std::size_t slash = authority.find('/');
            const std::string_view hostPort = slash == std::string_view::npos ? authority : authority.substr(0, slash);
            return hostIsLocal(hostPort);
        }
    }
    return false;
}

bool protocolVersionSupported(std::string_view version) {
    return version == "2025-06-18" || version == "2025-11-25" || version == "2025-03-26" || version == "2024-11-05";
}

McpHttpReply plain(int status, std::string body) {
    return McpHttpReply{status, "text/plain; charset=utf-8", std::move(body), {}};
}

// Runs every check that can refuse a request before its JSON-RPC body is
// dispatched. Returns the refusal, or nullopt with `body` set to the JSON-RPC
// text that should be dispatched.
std::optional<McpHttpReply> preflight(std::string_view raw, std::string_view expectedToken,
                                      std::string_view& body) {
    if (raw.size() > kMcpMaxRequestBytes) {
        return plain(413, "The request is larger than Kestrel accepts.");
    }
    bool malformed = false;
    const std::optional<ParsedHead> head = parseHead(raw, malformed);
    if (!head || malformed) {
        return plain(400, "The HTTP request line or headers could not be read.");
    }

    const std::string* host = header(*head, "host");
    if (header(*head, "transfer-encoding") != nullptr) {
        return plain(400, "Transfer-Encoding is not supported; send a Content-Length instead.");
    }
    if (host == nullptr || !hostIsLocal(*host)) {
        return plain(403, "Kestrel only answers requests addressed to this machine.");
    }

    if (const std::string* origin = header(*head, "origin");
        origin != nullptr && !origin->empty() && !originIsLocal(*origin)) {
        return plain(403, "Cross-origin requests to the local MCP endpoint are refused.");
    }

    const std::string* authorization = header(*head, "authorization");
    constexpr std::string_view bearer = "Bearer ";
    if (expectedToken.empty() || authorization == nullptr ||
        authorization->compare(0, bearer.size(), bearer) != 0 ||
        !constantTimeEquals(std::string_view(*authorization).substr(bearer.size()), expectedToken)) {
        return plain(401, "A valid Kestrel MCP token is required.");
    }

    if (head->path != kEndpointPath) {
        return plain(404, "Kestrel serves MCP at /mcp only.");
    }
    if (head->method != "POST") {
        return plain(405, "This stateless MCP endpoint accepts POST only; SSE and session deletion are not offered.");
    }

    if (const std::string* version = header(*head, "mcp-protocol-version");
        version != nullptr && !version->empty() && !protocolVersionSupported(*version)) {
        return plain(400, "Unsupported MCP-Protocol-Version header.");
    }

    const std::string* length = header(*head, "content-length");
    if (length == nullptr) {
        return plain(411, "POST requests need a Content-Length header.");
    }
    std::size_t declared = 0;
    const auto [end, error] = std::from_chars(length->data(), length->data() + length->size(), declared);
    if (error != std::errc{} || end != length->data() + length->size()) {
        return plain(400, "Content-Length must be a plain number of bytes.");
    }
    if (head->body.size() < declared) {
        return plain(400, "The request body is shorter than its Content-Length.");
    }

    // The body may be longer than the declared length if a client pipelined;
    // only the declared bytes belong to this request.
    body = head->body.substr(0, declared);
    return std::nullopt;
}

} // namespace

bool mcpRequestComplete(std::string_view raw) {
    if (raw.size() > kMcpMaxRequestBytes) {
        return true; // answered with 413 rather than read further
    }
    bool malformed = false;
    const std::optional<ParsedHead> head = parseHead(raw, malformed);
    if (!head) {
        return false;
    }
    if (malformed) {
        return true;
    }
    const std::string* length = header(*head, "content-length");
    if (length == nullptr) {
        return true; // a body-less request is complete at the blank line
    }
    std::size_t expected = 0;
    const auto [end, error] = std::from_chars(length->data(), length->data() + length->size(), expected);
    if (error != std::errc{} || end != length->data() + length->size()) {
        return true; // a bad length is reported, not waited on
    }
    return head->body.size() >= expected;
}

void answerMcpRequestAsync(std::string_view raw, std::string_view expectedToken,
                           McpDispatcher& dispatcher, std::function<void(McpHttpReply)> done) {
    std::string_view body;
    if (std::optional<McpHttpReply> refusal = preflight(raw, expectedToken, body)) {
        done(std::move(*refusal));
        return;
    }
    dispatcher.handleAsync(body, [done = std::move(done)](std::string reply) {
        if (reply.empty()) {
            // A notification has no response body, which is the MCP convention.
            done(McpHttpReply{202, "application/json", {}, {}});
            return;
        }
        done(McpHttpReply{200, "application/json", std::move(reply), {}});
    });
}

McpHttpReply answerMcpRequest(std::string_view raw, std::string_view expectedToken,
                              McpDispatcher& dispatcher) {
    // Shared so a late completion cannot write into a finished stack frame.
    auto state = std::make_shared<std::pair<bool, McpHttpReply>>(false, McpHttpReply{});
    answerMcpRequestAsync(raw, expectedToken, dispatcher, [state](McpHttpReply reply) {
        state->first = true;
        state->second = std::move(reply);
    });
    if (!state->first) {
        return plain(503, "This request completes later; use the asynchronous endpoint.");
    }
    return state->second;
}

} // namespace kestrel::core
