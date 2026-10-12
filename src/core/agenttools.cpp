#include "core/agenttools.h"

#include <cstddef>

#include "storage/json.h"

namespace kestrel::core {

namespace json = storage::json;

namespace {

constexpr std::string_view kOpenTag = "<tool_call>";
constexpr std::string_view kCloseTag = "</tool_call>";

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

std::optional<ModelToolCall> findModelToolCall(std::string_view text) {
    const std::size_t open = text.find(kOpenTag);
    if (open == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t payloadStart = open + kOpenTag.size();
    const std::size_t close = text.find(kCloseTag, payloadStart);
    if (close == std::string_view::npos) {
        return std::nullopt; // still being written
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
