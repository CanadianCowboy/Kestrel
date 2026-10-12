#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "core/idletool.h"
#include "storage/json.h"

namespace kestrel::core {

// What a tool handler reports back. A tool that fails for an ordinary reason
// (bad input, an unavailable resource) returns isError rather than throwing:
// the MCP client is meant to see the failure and reason about it.
struct McpToolResult {
    bool isError = false;
    std::string text;
};

// Delivers a tool's result. An asynchronous tool may call it later, on the
// same thread, after the request has been accepted.
using McpCompletion = std::function<void(McpToolResult)>;
// Receives a response body. An empty body means no reply is due (a
// notification).
using McpReply = std::function<void(std::string)>;

// One tool an external agent may call. The permissions it lists are the same
// closed set the idle loop uses, so an MCP tool cannot reach anything the idle
// tools could not. Registration declares the tool disabled; the user has to
// enable it and grant each permission before a call is allowed.
//
// A tool supplies exactly one of the two handlers. `handler` answers at once.
// `asyncHandler` answers when its completion runs, which may be much later
// (for example after a person has approved the action). Only the asynchronous
// form can be used with handleAsync; the synchronous form refuses it.
struct McpToolDefinition {
    std::string name;
    std::string description;
    std::vector<ToolPermission> permissions;
    std::function<McpToolResult(const storage::json::Value& arguments)> handler;
    std::function<void(const storage::json::Value& arguments, McpCompletion done)> asyncHandler;
};

// What a tool is and whether the user has switched it on, for describing the
// app's capabilities to the model.
struct McpToolSummary {
    std::string name;
    std::string description;
    std::vector<ToolPermission> permissions;
    bool enabled = false;
    bool permitted = false;
};

// Turns JSON-RPC request bodies into response bodies.
//
// Portable on purpose: it knows nothing about sockets, HTTP, or Qt. The
// transport decides how bytes arrive and how authentication is checked; this
// class decides what a well-formed request means. That split is what lets the
// protocol be tested without opening a port.
class McpDispatcher {
public:
    static constexpr std::string_view kProtocolVersion = "2025-06-18";

    // Declares a tool, replacing an earlier one with the same name. The tool
    // starts disabled and ungranted.
    void addTool(McpToolDefinition tool);

    // The permission state the user controls. Enabling and granting go through
    // here so the same registry governs idle tools and external tools.
    [[nodiscard]] IdleToolRegistry& permissions() noexcept { return m_permissions; }
    [[nodiscard]] const IdleToolRegistry& permissions() const noexcept { return m_permissions; }

    // Handles one request and delivers its reply, possibly later. Replies are
    // delivered exactly once.
    void handleAsync(std::string_view requestBody, McpReply reply);

    // Every registered tool with its current state, in registration order.
    [[nodiscard]] std::vector<McpToolSummary> toolSummaries() const;

    // Synchronous convenience for callers that cannot wait. An asynchronous
    // tool that has not finished by the time this returns yields an error body.
    [[nodiscard]] std::string handle(std::string_view requestBody);

private:
    [[nodiscard]] storage::json::Value dispatch(const storage::json::Value& request, bool& isNotification);
    [[nodiscard]] storage::json::Value callTool(const storage::json::Value& params);
    [[nodiscard]] storage::json::Value listTools() const;
    [[nodiscard]] const McpToolDefinition* findTool(const std::string& name) const;

    std::vector<McpToolDefinition> m_tools;
    IdleToolRegistry m_permissions;
};

// Plain-language description of what Kestrel can do for an agent, for the
// model's own system prompt. It states what is enabled, and it is honest about
// what the model itself cannot do from a chat turn.
[[nodiscard]] std::string describeAgentCapabilities(const McpDispatcher& dispatcher,
                                                    bool endpointRunning);

} // namespace kestrel::core
