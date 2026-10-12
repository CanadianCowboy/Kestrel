#include "core/mcpdispatch.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace kestrel::core {

namespace json = storage::json;

namespace {

// JSON-RPC 2.0 reserved error codes.
constexpr std::int64_t kParseError = -32700;
constexpr std::int64_t kInvalidRequest = -32600;
constexpr std::int64_t kMethodNotFound = -32601;
constexpr std::int64_t kInvalidParams = -32602;

json::Value response(const json::Value& id, json::Value body, const char* key) {
    json::Value envelope = json::Value::makeObject();
    envelope.set("jsonrpc", json::Value::makeString("2.0"));
    envelope.set("id", id);
    envelope.set(key, std::move(body));
    return envelope;
}

json::Value errorBody(std::int64_t code, std::string message) {
    json::Value error = json::Value::makeObject();
    error.set("code", json::Value::makeInteger(code));
    error.set("message", json::Value::makeString(std::move(message)));
    return error;
}

json::Value textContent(const std::string& text, bool isError) {
    json::Value item = json::Value::makeObject();
    item.set("type", json::Value::makeString("text"));
    item.set("text", json::Value::makeString(text));
    json::Value content = json::Value::makeArray();
    content.push(std::move(item));
    json::Value result = json::Value::makeObject();
    result.set("content", std::move(content));
    result.set("isError", json::Value::makeBoolean(isError));
    return result;
}

std::string joinPermissions(const std::vector<ToolPermission>& permissions) {
    std::string joined;
    for (const ToolPermission permission : permissions) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += toString(permission);
    }
    return joined;
}

} // namespace

void McpDispatcher::addTool(McpToolDefinition tool) {
    IdleToolDeclaration declaration;
    declaration.name = tool.name;
    declaration.summary = tool.description;
    declaration.permissions = tool.permissions;
    // Enabled-by-default is refused on purpose: an external agent gets nothing
    // until the user has switched the tool on and granted what it needs.
    declaration.enabledByDefault = false;
    m_permissions.declare(std::move(declaration));

    const auto existing = std::find_if(m_tools.begin(), m_tools.end(),
                                       [&tool](const McpToolDefinition& candidate) {
                                           return candidate.name == tool.name;
                                       });
    if (existing != m_tools.end()) {
        *existing = std::move(tool);
    } else {
        m_tools.push_back(std::move(tool));
    }
}

const McpToolDefinition* McpDispatcher::findTool(const std::string& name) const {
    const auto found = std::find_if(m_tools.begin(), m_tools.end(),
                                    [&name](const McpToolDefinition& candidate) {
                                        return candidate.name == name;
                                    });
    return found == m_tools.end() ? nullptr : &*found;
}

void McpDispatcher::handleAsync(std::string_view requestBody, McpReply reply) {
    std::string error;
    std::optional<json::Value> request = json::Value::parse(requestBody, error);
    if (!request) {
        // No id can be known for an unparseable body, so the reply carries null.
        reply(response(json::Value::makeNull(),
                       errorBody(kParseError, "The request is not valid JSON: " + error), "error")
                  .serialize());
        return;
    }
    if (!request->isObject()) {
        reply(response(json::Value::makeNull(),
                       errorBody(kInvalidRequest, "A JSON-RPC request must be an object."), "error")
                  .serialize());
        return;
    }

    // An asynchronous tool is started here rather than inside dispatch, because
    // its reply is not available until later. The permission check is the same
    // one callTool applies, so an unpermitted call is still refused at once.
    const json::Value* method = request->find("method");
    const json::Value* params = request->find("params");
    if (method != nullptr && method->isString() && *method->asString() == "tools/call" &&
        params != nullptr && params->isObject()) {
        const json::Value* nameField = params->find("name");
        const McpToolDefinition* tool =
            nameField != nullptr && nameField->isString() ? findTool(std::string(*nameField->asString()))
                                                          : nullptr;
        if (tool != nullptr && tool->asyncHandler && m_permissions.permits(tool->name)) {
            const json::Value* idField = request->find("id");
            const bool isNotification = idField == nullptr;
            const json::Value id = isNotification ? json::Value::makeNull() : *idField;
            const json::Value* arguments = params->find("arguments");
            const json::Value args = arguments != nullptr ? *arguments : json::Value::makeObject();
            tool->asyncHandler(args, [id, isNotification, reply = std::move(reply)](McpToolResult result) {
                if (isNotification) {
                    reply(std::string{});
                    return;
                }
                reply(response(id, textContent(result.text, result.isError), "result").serialize());
            });
            return;
        }
    }

    bool isNotification = false;
    json::Value out = dispatch(*request, isNotification);
    reply(isNotification ? std::string{} : out.serialize());
}

std::string McpDispatcher::handle(std::string_view requestBody) {
    // Shared so a late completion from an asynchronous tool cannot write into a
    // stack frame that has already returned.
    auto state = std::make_shared<std::pair<bool, std::string>>(false, std::string{});
    handleAsync(requestBody, [state](std::string body) {
        state->first = true;
        state->second = std::move(body);
    });
    if (!state->first) {
        return response(json::Value::makeNull(),
                        errorBody(kInvalidRequest,
                                  "This call completes later and needs the asynchronous endpoint."),
                        "error")
            .serialize();
    }
    return state->second;
}

json::Value McpDispatcher::dispatch(const json::Value& request, bool& isNotification) {
    const json::Value* idField = request.find("id");
    isNotification = idField == nullptr;
    const json::Value id = idField != nullptr ? *idField : json::Value::makeNull();

    const json::Value* jsonrpc = request.find("jsonrpc");
    const json::Value* method = request.find("method");
    if (jsonrpc == nullptr || !jsonrpc->isString() || jsonrpc->asString() != "2.0" ||
        method == nullptr || !method->isString()) {
        return response(id, errorBody(kInvalidRequest,
                                      "Requests need \"jsonrpc\": \"2.0\" and a string \"method\"."),
                        "error");
    }

    const std::string_view name = *method->asString();
    const json::Value* params = request.find("params");

    if (name == "initialize") {
        json::Value capabilities = json::Value::makeObject();
        capabilities.set("tools", json::Value::makeObject());
        json::Value serverInfo = json::Value::makeObject();
        serverInfo.set("name", json::Value::makeString("kestrel"));
        serverInfo.set("version", json::Value::makeString("0.1"));
        json::Value result = json::Value::makeObject();
        result.set("protocolVersion", json::Value::makeString(std::string(kProtocolVersion)));
        result.set("capabilities", std::move(capabilities));
        result.set("serverInfo", std::move(serverInfo));
        return response(id, std::move(result), "result");
    }
    if (name == "ping") {
        return response(id, json::Value::makeObject(), "result");
    }
    if (name == "notifications/initialized") {
        isNotification = true;
        return json::Value::makeNull();
    }
    if (name == "tools/list") {
        return response(id, listTools(), "result");
    }
    if (name == "tools/call") {
        if (params == nullptr || !params->isObject()) {
            return response(id, errorBody(kInvalidParams, "tools/call needs an object of params."),
                            "error");
        }
        return response(id, callTool(*params), "result");
    }
    return response(id, errorBody(kMethodNotFound, "Kestrel does not implement \"" +
                                                       std::string(name) + "\"."),
                    "error");
}

std::vector<McpToolSummary> McpDispatcher::toolSummaries() const {
    std::vector<McpToolSummary> summaries;
    summaries.reserve(m_tools.size());
    for (const McpToolDefinition& tool : m_tools) {
        McpToolSummary summary;
        summary.name = tool.name;
        summary.description = tool.description;
        summary.permissions = tool.permissions;
        summary.enabled = m_permissions.enabled(tool.name);
        summary.permitted = m_permissions.permits(tool.name);
        summaries.push_back(std::move(summary));
    }
    return summaries;
}

std::string describeAgentCapabilities(const McpDispatcher& dispatcher, bool endpointRunning) {
    std::string text = "Kestrel's own capabilities:\n";
    if (endpointRunning) {
        text += "- Other local agents can reach Kestrel through an MCP endpoint on this machine. "
                "Each tool below is offered only when the user has enabled it.\n";
    } else {
        text += "- The MCP endpoint is not running this session, so no outside agent can reach Kestrel.\n";
    }

    const std::vector<McpToolSummary> tools = dispatcher.toolSummaries();
    const bool anyEnabled = std::any_of(tools.begin(), tools.end(),
                                        [](const McpToolSummary& tool) { return tool.enabled; });
    if (!anyEnabled) {
        text += "- No external tools are enabled right now.\n";
    }
    for (const McpToolSummary& tool : tools) {
        if (!tool.enabled) {
            continue;
        }
        text += "- Tool \"" + tool.name + "\": " + tool.description;
        if (!tool.permitted) {
            text += " (enabled, but a required permission is not granted, so calls are refused)";
        }
        text += "\n";
    }

    text += "- Administrator commands are never run silently. Each one shows the user the exact "
            "command and the reason, and runs only after they approve it.\n";
    text += "- To use a tool from this chat, reply with exactly one call and then stop, in the form "
            "<tool_call>{\"name\":\"TOOL\",\"arguments\":{...}}</tool_call>. Kestrel runs it and sends "
            "the result back in the next message. Call only the tools listed above, or \"list_tools\" to see "
            "what is enabled now. Use a tool only when it helps answer the user, and never state a result "
            "you have not received. Commands and files are not available except through the listed tools.";
    return text;
}

json::Value McpDispatcher::listTools() const {
    json::Value tools = json::Value::makeArray();
    for (const McpToolDefinition& tool : m_tools) {
        // Only tools the user has switched on are offered, so a client never
        // sees a capability the user has not agreed to expose.
        if (!m_permissions.enabled(tool.name)) {
            continue;
        }
        json::Value entry = json::Value::makeObject();
        entry.set("name", json::Value::makeString(tool.name));
        entry.set("description", json::Value::makeString(tool.description));
        json::Value schema = json::Value::makeObject();
        schema.set("type", json::Value::makeString("object"));
        entry.set("inputSchema", std::move(schema));
        tools.push(std::move(entry));
    }
    json::Value result = json::Value::makeObject();
    result.set("tools", std::move(tools));
    return result;
}

json::Value McpDispatcher::callTool(const json::Value& params) {
    const json::Value* nameField = params.find("name");
    if (nameField == nullptr || !nameField->isString()) {
        return textContent("tools/call needs a string \"name\".", true);
    }
    const std::string name(*nameField->asString());

    const McpToolDefinition* tool = findTool(name);
    if (tool == nullptr) {
        return textContent("No tool named \"" + name + "\" is registered.", true);
    }

    // Refused tools say what is missing, so the user (or the calling agent)
    // can tell a disabled tool from one that is merely unauthorised.
    if (!m_permissions.permits(name)) {
        if (!m_permissions.enabled(name)) {
            return textContent("Tool \"" + name + "\" is disabled. Enable it in Kestrel first.", true);
        }
        const std::vector<ToolPermission> missing = m_permissions.missing(name);
        return textContent("Tool \"" + name + "\" needs permission not granted: " +
                               joinPermissions(missing) + ".",
                           true);
    }

    if (!tool->handler) {
        // Reached only when an asynchronous tool is called through the
        // synchronous path, which has no way to wait for its reply.
        return textContent("Tool \"" + name + "\" completes later and cannot be answered here.", true);
    }
    const json::Value* arguments = params.find("arguments");
    json::Value empty = json::Value::makeObject();
    const McpToolResult outcome = tool->handler(arguments != nullptr ? *arguments : empty);
    return textContent(outcome.text, outcome.isError);
}

} // namespace kestrel::core
