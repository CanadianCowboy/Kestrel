#include "core/agenttools.h"

#include <cstddef>
#include <algorithm>
#include <cctype>

#include "storage/json.h"

namespace kestrel::core {

namespace json = storage::json;

namespace {

constexpr std::string_view kOpenTag = "<tool_call>";
constexpr std::string_view kCloseTag = "</tool_call>";

// The end of the JSON object that starts at `start`, or npos while it is still
// being written. Strings and their escapes are skipped, so a brace inside a
// value does not end the object early.
std::size_t objectEnd(std::string_view text, std::size_t start) {
    if (start >= text.size() || text[start] != '{') {
        return std::string_view::npos;
    }
    int depth = 0;
    bool inString = false;
    for (std::size_t i = start; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (c == '\\') {
                ++i; // skip the escaped character
            } else if (c == '"') {
                inString = false;
            }
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{') {
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0) {
                return i + 1;
            }
        }
    }
    return std::string_view::npos;
}

// A call written without the tags. A small base model shown the protocol once
// tends to answer with the bare JSON object instead, and refusing it means the
// user sees the call text and a made-up result restated as an answer.
//
// Only the reply's own leading object counts: the same JSON inside a sentence
// or a code sample is text, not an action.
std::optional<ModelToolCall> parseBareCall(std::string_view text) {
    const std::size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string_view::npos || text[start] != '{') {
        return std::nullopt;
    }
    const std::size_t end = objectEnd(text, start);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    std::string error;
    const std::optional<json::Value> call = json::Value::parse(text.substr(start, end - start), error);
    if (!call || !call->isObject()) {
        return std::nullopt;
    }
    const json::Value* name = call->find("name");
    if (name == nullptr || !name->isString() || name->asString()->empty()) {
        return std::nullopt;
    }
    ModelToolCall result;
    result.name = std::string(*name->asString());
    const json::Value* arguments = call->find("arguments");
    result.argumentsJson = arguments != nullptr ? arguments->serialize() : std::string("{}");
    return result;
}

std::optional<std::vector<WebSearchHit>> parseResultList(std::string_view body, std::string& error) {
    std::string parseError;
    const std::optional<json::Value> document = json::Value::parse(body, parseError);
    if (!document) {
        error = "The search response was not valid JSON: " + parseError;
        return std::nullopt;
    }
    const json::Value* results = document->find("results");
    if (results == nullptr || results->asArray() == nullptr) {
        error = "The search response had no results list.";
        return std::nullopt;
    }
    std::vector<WebSearchHit> hits;
    for (const json::Value& item : *results->asArray()) {
        WebSearchHit hit;
        if (const json::Value* title = item.find("title"); title != nullptr && title->isString()) {
            hit.title = std::string(*title->asString());
        }
        if (const json::Value* url = item.find("url"); url != nullptr && url->isString()) {
            hit.url = std::string(*url->asString());
        }
        if (const json::Value* content = item.find("content"); content != nullptr && content->isString()) {
            hit.snippet = std::string(*content->asString());
        }
        // A result with no address cannot be followed, so it is dropped rather
        // than handed to the model as if it were a source.
        if (!hit.url.empty()) {
            hits.push_back(std::move(hit));
        }
    }
    error.clear();
    return hits;
}

} // namespace

bool modelReplyNeedsMoreJson(std::string_view text) {
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    return first == std::string_view::npos
        || (text[first] == '{' && objectEnd(text, first) == std::string_view::npos);
}

std::optional<ModelToolCall> findModelToolCall(std::string_view text) {
    const std::size_t open = text.find(kOpenTag);
    if (open == std::string_view::npos) {
        return parseBareCall(text);
    }
    const std::size_t payloadStart = open + kOpenTag.size();
    const std::size_t close = text.find(kCloseTag, payloadStart);
    if (close == std::string_view::npos) {
        // The JSON payload is sufficient to commit the call. Qwen may emit
        // the closing tag as a special end token, which is not delivered as
        // text by the backend. Do not wait for that token or trailing prose.
        return parseBareCall(text.substr(payloadStart));
    }
    std::string error;
    const std::optional<json::Value> call =
        json::Value::parse(text.substr(payloadStart, close - payloadStart), error);
    if (!call || !call->isObject()) {
        return std::nullopt;
    }
    const json::Value* name = call->find("name");
    if (name == nullptr || !name->isString() || name->asString()->empty()) {
        return std::nullopt;
    }
    ModelToolCall result;
    result.name = std::string(*name->asString());
    const json::Value* arguments = call->find("arguments");
    result.argumentsJson = arguments != nullptr ? arguments->serialize() : std::string("{}");
    return result;
}

bool shouldRouteTools(std::string_view prompt, bool hasEnabledTools, bool force) {
    if (!hasEnabledTools) { return false; }
    if (force) { return true; }
    std::string text(prompt);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (text.find("what tools") != std::string::npos || text.find("what can you do") != std::string::npos
        || text.find("which tools") != std::string::npos) { return false; }
    for (const std::string_view phrase : {"undermind", "workspace", "search the web", "look up", "browse",
            "open ", "load ", "run ", "execute ", "read file", "list files", "current weather",
            "latest news", "julian", "convert ", "calculate ", "download ", "fetch "}) {
        if (text.find(phrase) != std::string::npos) { return true; }
    }
    return false;
}

bool needsInstructSynthesis(std::string_view prompt, int toolRounds) {
    if (toolRounds > 1) { return true; }
    std::string text(prompt);
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    for (const std::string_view phrase : {"compare", "synthesize", "analyse", "analyze", "table", "json", "multi-step"}) {
        if (text.find(phrase) != std::string::npos) { return true; }
    }
    return false;
}

std::optional<std::vector<WebSearchHit>> parseTavilyResults(std::string_view body, std::string& error) {
    return parseResultList(body, error);
}

std::optional<std::vector<WebSearchHit>> parseSearxngResults(std::string_view body, std::string& error) {
    return parseResultList(body, error);
}

std::string formatSearchHits(const std::vector<WebSearchHit>& hits) {
    if (hits.empty()) {
        return "No web results.";
    }
    std::string text;
    std::size_t number = 1;
    for (const WebSearchHit& hit : hits) {
        text += std::to_string(number++) + ". " + hit.title + "\n   " + hit.url + "\n";
        if (!hit.snippet.empty()) {
            text += "   " + hit.snippet + "\n";
        }
    }
    return text;
}

} // namespace kestrel::core
