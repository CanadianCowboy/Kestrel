#include "core/mcpdispatch.h"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

namespace kestrel::core {

namespace json = storage::json;

namespace {

// JSON-RPC 2.0 reserved error codes and MCP resource error code.
constexpr std::int64_t kParseError = -32700;
constexpr std::int64_t kInvalidRequest = -32600;
constexpr std::int64_t kMethodNotFound = -32601;
constexpr std::int64_t kInvalidParams = -32602;
constexpr std::int64_t kResourceNotFound = -32002;

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

bool McpDispatcher::removeTool(std::string_view name) {
    m_permissions.setEnabled(name, false);
    const auto it = std::find_if(m_tools.begin(), m_tools.end(),
                                 [name](const McpToolDefinition& candidate) {
                                     return candidate.name == name;
                                 });
    if (it == m_tools.end()) {
        return false;
    }
    m_tools.erase(it);
    return true;
}

void McpDispatcher::addResource(McpResourceDefinition resource) {
    IdleToolDeclaration declaration;
    declaration.name = resource.uri;
    declaration.summary = resource.description;
    declaration.permissions = resource.permissions;
    declaration.enabledByDefault = false;
    m_permissions.declare(std::move(declaration));

    const auto existing = std::find_if(m_resources.begin(), m_resources.end(),
                                       [&resource](const McpResourceDefinition& candidate) {
                                           return candidate.uri == resource.uri;
                                       });
    if (existing != m_resources.end()) {
        *existing = std::move(resource);
    } else {
        m_resources.push_back(std::move(resource));
    }
}

void McpDispatcher::addResourceTemplate(McpResourceTemplateDefinition resourceTemplate) {
    IdleToolDeclaration declaration;
    declaration.name = resourceTemplate.uriTemplate;
    declaration.summary = resourceTemplate.description;
    declaration.permissions = resourceTemplate.permissions;
    declaration.enabledByDefault = false;
    m_permissions.declare(std::move(declaration));

    const auto existing = std::find_if(
        m_resourceTemplates.begin(), m_resourceTemplates.end(),
        [&resourceTemplate](const McpResourceTemplateDefinition& candidate) {
            return candidate.uriTemplate == resourceTemplate.uriTemplate;
        });
    if (existing != m_resourceTemplates.end()) {
        *existing = std::move(resourceTemplate);
    } else {
        m_resourceTemplates.push_back(std::move(resourceTemplate));
    }
}

void McpDispatcher::addPrompt(McpPromptDefinition prompt) {
    IdleToolDeclaration declaration;
    declaration.name = prompt.name;
    declaration.summary = prompt.description;
    declaration.permissions = prompt.permissions;
    declaration.enabledByDefault = false;
    m_permissions.declare(std::move(declaration));

    const auto existing = std::find_if(m_prompts.begin(), m_prompts.end(),
                                       [&prompt](const McpPromptDefinition& candidate) {
                                           return candidate.name == prompt.name;
                                       });
    if (existing != m_prompts.end()) {
        *existing = std::move(prompt);
    } else {
        m_prompts.push_back(std::move(prompt));
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

    const json::Value* version = request->find("jsonrpc");
    const json::Value* requestId = request->find("id");
    const json::Value* requestMethod = request->find("method");
    const json::Value* requestParams = request->find("params");
    if (version == nullptr || version->asString() != "2.0"
        || requestMethod == nullptr || !requestMethod->isString()
        || (requestId != nullptr && !requestId->isString() && !requestId->isInteger())
        || (requestParams != nullptr && !requestParams->isObject())) {
        reply(response(json::Value::makeNull(), errorBody(kInvalidRequest,
                       "Expected a JSON-RPC 2.0 method, string/integer ID, and object params."), "error").serialize());
        return;
    }
    if (*requestMethod->asString() == "tools/call" && requestParams != nullptr) {
        const json::Value* arguments = requestParams->find("arguments");
        if (arguments != nullptr && !arguments->isObject()) {
            reply(requestId != nullptr
                ? response(*requestId, errorBody(kInvalidParams, "Tool arguments must be an object."), "error").serialize()
                : std::string{});
            return;
        }
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
            auto completed = std::make_shared<bool>(false);
            tool->asyncHandler(args, [id, isNotification, completed, reply = std::move(reply)](McpToolResult result) {
                if (std::exchange(*completed, true)) { return; }
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
        json::Value toolsCap = json::Value::makeObject();
        toolsCap.set("listChanged", json::Value::makeBoolean(false));
        json::Value resourcesCap = json::Value::makeObject();
        resourcesCap.set("subscribe", json::Value::makeBoolean(false));
        resourcesCap.set("listChanged", json::Value::makeBoolean(false));
        json::Value promptsCap = json::Value::makeObject();
        promptsCap.set("listChanged", json::Value::makeBoolean(false));

        json::Value capabilities = json::Value::makeObject();
        capabilities.set("tools", std::move(toolsCap));
        capabilities.set("resources", std::move(resourcesCap));
        capabilities.set("prompts", std::move(promptsCap));
        capabilities.set("completions", json::Value::makeObject());
        capabilities.set("logging", json::Value::makeObject());

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
    if (name == "notifications/initialized" || name == "notifications/cancelled" ||
        name == "notifications/roots/list_changed" || name == "notifications/progress") {
        isNotification = true;
        return json::Value::makeNull();
    }
    if (name == "logging/setLevel") {
        if (params == nullptr || !params->isObject()) {
            return response(id, errorBody(kInvalidParams, "logging/setLevel needs an object of params."),
                            "error");
        }
        const json::Value* level = params->find("level");
        if (level == nullptr || !level->isString()) {
            return response(id, errorBody(kInvalidParams, "logging/setLevel needs a string \"level\"."),
                            "error");
        }
        const std::string_view value = *level->asString();
        constexpr std::string_view validLevels[] = {
            "debug", "info", "notice", "warning", "error", "critical", "alert", "emergency"};
        const bool known = std::any_of(std::begin(validLevels), std::end(validLevels),
                                       [value](std::string_view candidate) { return candidate == value; });
        if (!known) {
            return response(id, errorBody(kInvalidParams, "Unknown MCP log level."), "error");
        }
        m_logLevel = std::string(value);
        return response(id, json::Value::makeObject(), "result");
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
    if (name == "resources/list") {
        return response(id, listResources(), "result");
    }
    if (name == "resources/templates/list") {
        return response(id, listResourceTemplates(), "result");
    }
    if (name == "resources/read") {
        if (params == nullptr || !params->isObject()) {
            return response(id, errorBody(kInvalidParams, "resources/read needs an object of params."),
                            "error");
        }
        return readResource(id, *params);
    }
    if (name == "prompts/list") {
        return response(id, listPrompts(), "result");
    }
    if (name == "prompts/get") {
        if (params == nullptr || !params->isObject()) {
            return response(id, errorBody(kInvalidParams, "prompts/get needs an object of params."),
                            "error");
        }
        return getPrompt(id, *params);
    }
    if (name == "completion/complete") {
        if (params == nullptr || !params->isObject()) {
            return response(id, errorBody(kInvalidParams, "completion/complete needs an object of params."),
                            "error");
        }
        return completeArgument(id, *params);
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

std::string describeAgentCapabilities(const McpDispatcher& dispatcher, bool endpointRunning,
                                      std::size_t maxChars) {
    std::string text = "Kestrel's own capabilities:\n";
    if (endpointRunning) {
        text += "- Other local agents can reach Kestrel through an MCP endpoint on this machine. "
                "Each tool below is offered only when the user has enabled it.\n";
    } else {
        text += "- The MCP endpoint is not running this session, so no outside agent can reach Kestrel.\n";
    }

    const std::vector<McpToolSummary> tools = dispatcher.toolSummaries();
    // A tool that declares no permission is one of Kestrel's own local
    // computations, not an outside capability. Counting only permissioned
    // tools keeps "nothing external is enabled" true when the only thing on
    // offer is local arithmetic.
    const bool anyExternal = std::any_of(tools.begin(), tools.end(),
                                         [](const McpToolSummary& tool) {
                                             return tool.enabled && !tool.permissions.empty();
                                         });
    if (!anyExternal) {
        text += "- No external tools are enabled right now. The tools listed below are built into "
                "Kestrel and run locally.\n";
    }
    // The closing instructions are added after the list, so part of the
    // budget is reserved for them rather than discovered too late.
    const std::size_t reserved = 1000;
    const std::size_t listBudget = maxChars > reserved ? maxChars - reserved : maxChars;
    std::vector<const McpToolSummary*> enabledTools;
    for (const McpToolSummary& tool : tools) {
        if (tool.enabled) {
            enabledTools.push_back(&tool);
        }
    }
    const auto toolPriority = [](const McpToolSummary* tool) {
        const std::string_view name = tool->name;
        const bool isUndermind = name.starts_with("undermind_");
        const bool isWorkspace = name.find("workspace") != std::string_view::npos
            || name.find("project") != std::string_view::npos
            || name.find("load") != std::string_view::npos
            || name.find("list") != std::string_view::npos;
        if (isUndermind && isWorkspace) {
            return 0;
        }
        if (isUndermind) {
            return 1;
        }
        if (isWorkspace) {
            return 2;
        }
        return 3;
    };
    std::stable_sort(enabledTools.begin(), enabledTools.end(),
                     [&toolPriority](const McpToolSummary* a, const McpToolSummary* b) {
                         return toolPriority(a) < toolPriority(b);
                     });
    std::size_t omitted = 0;
    for (const McpToolSummary* toolPtr : enabledTools) {
        const McpToolSummary& tool = *toolPtr;
        std::string description = tool.description;
        if (description.size() > kAgentCapabilityToolChars) {
            description.resize(kAgentCapabilityToolChars);
            description += " ...";
        }
        std::string line = "- Tool \"" + tool.name + "\": " + description;
        if (!tool.permitted) {
            line += " (enabled, but a required permission is not granted, so calls are refused)";
        }
        line += "\n";
        if (text.size() + line.size() > listBudget) {
            ++omitted;
            continue;
        }
        text += line;
    }
    if (omitted > 0) {
        text += "- " + std::to_string(omitted)
            + " more enabled tool(s) do not fit in this prompt. Call \"list_tools\" to see every "
              "enabled tool by name before deciding one is unavailable.\n";
    }

    text += "- Administrator commands are never run silently: each one shows the command and the "
            "reason, and runs only after approval.\n";
    // list_tools is only worth mentioning when the list above is incomplete.
    // Advertised beside a complete list, a base model calls it instead of
    // answering a question about its tools, and the round trip is what turns a
    // simple question into a stream of JSON.
    if (omitted > 0) {
        text += "- \"list_tools\" reports every enabled tool, including the ones that did not fit "
                "above. Look before saying a tool is unavailable.\n";
    } else {
        text += "- This list is complete: answer a question about your tools from it, in prose.\n";
    }
    // The argument signatures above are for calling a tool, not for reciting.
    // Left unsaid, a model answers "what can you do?" by reading the list back,
    // signatures and all, which reads as instructions instead of an answer.
    text += "- When the user asks what you can do, answer in two or three sentences of prose: what each "
            "tool is for, never its argument names or JSON, and do not call anything.\n";
    text += "- To use a tool, use it rather than describing it: reply with one "
            "<tool_call>{\"name\":\"TOOL\",\"arguments\":{...}}</tool_call> block and nothing after "
            "it, never inside an answer.\n";
    text += "- A tool result arrives as the next message: answer from it in prose, do not print it as "
            "JSON, do not repeat it, and do not repeat the same call. Never state a result you have "
            "not received. Commands and files are available only through the listed tools.";
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
        std::string parseError;
        std::optional<json::Value> parsedSchema =
            tool.inputSchemaJson.empty() ? std::nullopt : json::Value::parse(tool.inputSchemaJson, parseError);
        if (parsedSchema && parsedSchema->isObject()) {
            entry.set("inputSchema", std::move(*parsedSchema));
        } else {
            json::Value schema = json::Value::makeObject();
            schema.set("type", json::Value::makeString("object"));
            entry.set("inputSchema", std::move(schema));
        }
        tools.push(std::move(entry));
    }
    json::Value result = json::Value::makeObject();
    result.set("tools", std::move(tools));
    return result;
}

json::Value McpDispatcher::listResources() const {
    json::Value resources = json::Value::makeArray();
    for (const McpResourceDefinition& resource : m_resources) {
        if (!m_permissions.enabled(resource.uri)) {
            continue;
        }
        json::Value entry = json::Value::makeObject();
        entry.set("uri", json::Value::makeString(resource.uri));
        entry.set("name", json::Value::makeString(resource.name));
        entry.set("description", json::Value::makeString(resource.description));
        entry.set("mimeType", json::Value::makeString(resource.mimeType));
        resources.push(std::move(entry));
    }
    json::Value result = json::Value::makeObject();
    result.set("resources", std::move(resources));
    return result;
}

json::Value McpDispatcher::listResourceTemplates() const {
    json::Value templates = json::Value::makeArray();
    for (const McpResourceTemplateDefinition& tmpl : m_resourceTemplates) {
        if (!m_permissions.enabled(tmpl.uriTemplate)) {
            continue;
        }
        json::Value entry = json::Value::makeObject();
        entry.set("uriTemplate", json::Value::makeString(tmpl.uriTemplate));
        entry.set("name", json::Value::makeString(tmpl.name));
        entry.set("description", json::Value::makeString(tmpl.description));
        entry.set("mimeType", json::Value::makeString(tmpl.mimeType));
        templates.push(std::move(entry));
    }
    json::Value result = json::Value::makeObject();
    result.set("resourceTemplates", std::move(templates));
    return result;
}

json::Value McpDispatcher::readResource(const json::Value& id, const json::Value& params) const {
    const json::Value* uriField = params.find("uri");
    if (uriField == nullptr || !uriField->isString() || uriField->asString()->empty()) {
        return response(id, errorBody(kInvalidParams, "resources/read needs a non-empty string \"uri\"."),
                        "error");
    }
    const std::string uri(*uriField->asString());

    for (const McpResourceDefinition& resource : m_resources) {
        if (resource.uri != uri) {
            continue;
        }
        if (!m_permissions.permits(resource.uri)) {
            if (!m_permissions.enabled(resource.uri)) {
                return response(id, errorBody(kInvalidParams,
                                              "Resource \"" + uri + "\" is disabled. Enable it in Kestrel first."),
                                "error");
            }
            const std::vector<ToolPermission> missing = m_permissions.missing(resource.uri);
            return response(id, errorBody(kInvalidParams,
                                          "Resource \"" + uri + "\" needs permission not granted: " +
                                              joinPermissions(missing) + "."),
                            "error");
        }
        if (!resource.handler) {
            return response(id, errorBody(kInvalidParams, "Resource \"" + uri + "\" has no reader."),
                            "error");
        }
        const McpResourceContent content = resource.handler();
        if (content.isError) {
            return response(id, errorBody(kInvalidParams, content.text), "error");
        }
        json::Value item = json::Value::makeObject();
        item.set("uri", json::Value::makeString(content.uri.empty() ? uri : content.uri));
        item.set("mimeType", json::Value::makeString(content.mimeType.empty() ? resource.mimeType : content.mimeType));
        item.set("text", json::Value::makeString(content.text));
        json::Value contents = json::Value::makeArray();
        contents.push(std::move(item));
        json::Value result = json::Value::makeObject();
        result.set("contents", std::move(contents));
        return response(id, std::move(result), "result");
    }

    for (const McpResourceTemplateDefinition& tmpl : m_resourceTemplates) {
        const std::size_t brace = tmpl.uriTemplate.find('{');
        const std::string_view prefix = brace == std::string::npos
            ? std::string_view(tmpl.uriTemplate)
            : std::string_view(tmpl.uriTemplate).substr(0, brace);
        if (prefix.empty() || !std::string_view(uri).starts_with(prefix) || uri.size() <= prefix.size()) {
            continue;
        }
        if (!m_permissions.permits(tmpl.uriTemplate)) {
            if (!m_permissions.enabled(tmpl.uriTemplate)) {
                return response(id, errorBody(kInvalidParams,
                                              "Resource template \"" + tmpl.uriTemplate +
                                                  "\" is disabled. Enable it in Kestrel first."),
                                "error");
            }
            const std::vector<ToolPermission> missing = m_permissions.missing(tmpl.uriTemplate);
            return response(id, errorBody(kInvalidParams,
                                          "Resource template \"" + tmpl.uriTemplate +
                                              "\" needs permission not granted: " +
                                              joinPermissions(missing) + "."),
                            "error");
        }
        if (!tmpl.handler) {
            continue;
        }
        const McpResourceContent content = tmpl.handler(uri);
        if (content.isError) {
            return response(id, errorBody(kInvalidParams, content.text), "error");
        }
        json::Value item = json::Value::makeObject();
        item.set("uri", json::Value::makeString(content.uri.empty() ? uri : content.uri));
        item.set("mimeType", json::Value::makeString(content.mimeType.empty() ? tmpl.mimeType : content.mimeType));
        item.set("text", json::Value::makeString(content.text));
        json::Value contents = json::Value::makeArray();
        contents.push(std::move(item));
        json::Value result = json::Value::makeObject();
        result.set("contents", std::move(contents));
        return response(id, std::move(result), "result");
    }

    json::Value error = errorBody(kResourceNotFound, "Resource not found");
    json::Value data = json::Value::makeObject();
    data.set("uri", json::Value::makeString(uri));
    error.set("data", std::move(data));
    return response(id, std::move(error), "error");
}

json::Value McpDispatcher::listPrompts() const {
    json::Value prompts = json::Value::makeArray();
    for (const McpPromptDefinition& prompt : m_prompts) {
        if (!m_permissions.enabled(prompt.name)) {
            continue;
        }
        json::Value entry = json::Value::makeObject();
        entry.set("name", json::Value::makeString(prompt.name));
        entry.set("description", json::Value::makeString(prompt.description));
        json::Value arguments = json::Value::makeArray();
        for (const McpPromptArgument& arg : prompt.arguments) {
            json::Value item = json::Value::makeObject();
            item.set("name", json::Value::makeString(arg.name));
            item.set("description", json::Value::makeString(arg.description));
            item.set("required", json::Value::makeBoolean(arg.required));
            arguments.push(std::move(item));
        }
        entry.set("arguments", std::move(arguments));
        prompts.push(std::move(entry));
    }
    json::Value result = json::Value::makeObject();
    result.set("prompts", std::move(prompts));
    return result;
}

json::Value McpDispatcher::getPrompt(const json::Value& id, const json::Value& params) const {
    const json::Value* nameField = params.find("name");
    if (nameField == nullptr || !nameField->isString() || nameField->asString()->empty()) {
        return response(id, errorBody(kInvalidParams, "prompts/get needs a non-empty string \"name\"."),
                        "error");
    }
    const std::string name(*nameField->asString());
    const auto found = std::find_if(m_prompts.begin(), m_prompts.end(),
                                    [&name](const McpPromptDefinition& candidate) {
                                        return candidate.name == name;
                                    });
    if (found == m_prompts.end()) {
        return response(id, errorBody(kInvalidParams, "No prompt named \"" + name + "\" is registered."),
                        "error");
    }
    if (!m_permissions.permits(found->name)) {
        if (!m_permissions.enabled(found->name)) {
            return response(id, errorBody(kInvalidParams,
                                          "Prompt \"" + name + "\" is disabled. Enable it in Kestrel first."),
                            "error");
        }
        const std::vector<ToolPermission> missing = m_permissions.missing(found->name);
        return response(id, errorBody(kInvalidParams,
                                      "Prompt \"" + name + "\" needs permission not granted: " +
                                          joinPermissions(missing) + "."),
                        "error");
    }
    const json::Value* arguments = params.find("arguments");
    if (arguments != nullptr && !arguments->isObject()) {
        return response(id, errorBody(kInvalidParams, "Prompt arguments must be an object of strings."), "error");
    }
    const json::Value args = arguments != nullptr ? *arguments : json::Value::makeObject();
    for (const auto& member : *args.asObject()) {
        if (!member.value.isString()) {
            return response(id, errorBody(kInvalidParams, "Prompt argument values must be strings."), "error");
        }
    }
    for (const McpPromptArgument& requiredArg : found->arguments) {
        if (!requiredArg.required) {
            continue;
        }
        const json::Value* supplied = args.find(requiredArg.name);
        if (supplied == nullptr || !supplied->isString() || supplied->asString()->empty()) {
            return response(id, errorBody(kInvalidParams,
                                          "Prompt \"" + name + "\" requires argument \"" + requiredArg.name + "\"."),
                            "error");
        }
    }
    if (!found->handler) {
        return response(id, errorBody(kInvalidParams, "Prompt \"" + name + "\" has no handler."), "error");
    }
    const McpPromptResult built = found->handler(args);
    if (built.isError) {
        return response(id, errorBody(kInvalidParams, built.description), "error");
    }
    json::Value messages = json::Value::makeArray();
    for (const McpPromptMessage& message : built.messages) {
        json::Value content = json::Value::makeObject();
        content.set("type", json::Value::makeString("text"));
        content.set("text", json::Value::makeString(message.text));
        json::Value row = json::Value::makeObject();
        row.set("role", json::Value::makeString(message.role));
        row.set("content", std::move(content));
        messages.push(std::move(row));
    }
    json::Value result = json::Value::makeObject();
    result.set("description", json::Value::makeString(built.description.empty() ? found->description : built.description));
    result.set("messages", std::move(messages));
    return response(id, std::move(result), "result");
}

json::Value McpDispatcher::completeArgument(const json::Value& id, const json::Value& params) const {
    const json::Value* ref = params.find("ref");
    const json::Value* argument = params.find("argument");
    if (ref == nullptr || !ref->isObject() || argument == nullptr || !argument->isObject()) {
        return response(id, errorBody(kInvalidParams, "completion/complete needs \"ref\" and \"argument\" objects."),
                        "error");
    }
    const json::Value* refType = ref->find("type");
    const json::Value* argValue = argument->find("value");
    const json::Value* argNameField = argument->find("name");
    if (refType == nullptr || !refType->isString()
        || (refType->asString() != "ref/prompt" && refType->asString() != "ref/resource")
        || argValue == nullptr || !argValue->isString()
        || argNameField == nullptr || !argNameField->isString()) {
        return response(id, errorBody(kInvalidParams, "Completion requires a valid reference and string argument name/value."), "error");
    }
    const std::string_view prefix = (argValue != nullptr && argValue->isString()) ? *argValue->asString() : std::string_view{};
    json::Value values = json::Value::makeArray();
    std::int64_t count = 0;

    if (refType != nullptr && refType->isString() && *refType->asString() == "ref/resource") {
        const json::Value* refUri = ref->find("uri");
        for (const McpResourceDefinition& resource : m_resources) {
            if (m_permissions.enabled(resource.uri) && std::string_view(resource.uri).starts_with(prefix)) {
                values.push(json::Value::makeString(resource.uri));
                ++count;
            }
        }
        if (refUri != nullptr && refUri->isString()) {
            for (const McpResourceTemplateDefinition& tmpl : m_resourceTemplates) {
                if (tmpl.uriTemplate == *refUri->asString() && m_permissions.enabled(tmpl.uriTemplate)
                    && std::string_view(tmpl.uriTemplate).starts_with(prefix)) {
                    values.push(json::Value::makeString(tmpl.uriTemplate));
                    ++count;
                }
            }
        }
    } else if (refType != nullptr && refType->isString() && *refType->asString() == "ref/prompt") {
        const json::Value* promptName = ref->find("name");
        const json::Value* argName = argument->find("name");
        if (promptName != nullptr && promptName->isString() && argName != nullptr && argName->isString()) {
            const auto found = std::find_if(m_prompts.begin(), m_prompts.end(),
                                            [promptName](const McpPromptDefinition& candidate) {
                                                return candidate.name == *promptName->asString();
                                            });
            if (found != m_prompts.end() && m_permissions.enabled(found->name)) {
                for (const McpPromptArgument& arg : found->arguments) {
                    if (arg.name != *argName->asString()) {
                        continue;
                    }
                    if (!arg.completions.empty()) {
                        for (const std::string& candidate : arg.completions) {
                            if (std::string_view(candidate).starts_with(prefix)) {
                                if (count < 100) { values.push(json::Value::makeString(candidate)); }
                                ++count;
                            }
                        }
                    } else if (std::string_view(arg.name).starts_with(prefix)) {
                        values.push(json::Value::makeString(arg.name));
                        ++count;
                    }
                }
            }
        }
    }

    json::Value completion = json::Value::makeObject();
    completion.set("values", std::move(values));
    completion.set("total", json::Value::makeInteger(count));
    completion.set("hasMore", json::Value::makeBoolean(count > 100));
    json::Value result = json::Value::makeObject();
    result.set("completion", std::move(completion));
    return response(id, std::move(result), "result");
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
