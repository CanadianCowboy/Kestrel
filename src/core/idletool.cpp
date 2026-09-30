#include "core/idletool.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>

namespace kestrel::core {

namespace {

// A tool is identified by its name and nothing else, so the lookup that the
// permission check depends on has to agree with the lookup that switching a
// tool on depends on.
auto findByName(std::vector<IdleToolDeclaration>& declarations, std::string_view name) {
    return std::find_if(declarations.begin(), declarations.end(),
                        [name](const IdleToolDeclaration& declaration) {
                            return declaration.name == name;
                        });
}

/// Returns whether the permission list contains the requested capability.
bool holds(const std::vector<ToolPermission>& permissions, ToolPermission permission) {
    return std::find(permissions.begin(), permissions.end(), permission) != permissions.end();
}

// The capabilities a tool could ask for, in the order they are declared above.
// Used to describe a tool the registry has never heard of.
constexpr ToolPermission kAllPermissions[] = {
    ToolPermission::ReadConversations,
    ToolPermission::RunGeneration,
    ToolPermission::WriteFiles,
    ToolPermission::Network,
};

} // namespace

/// Returns a readable permission name, or unknown for an unrecognized value.
const char* toString(ToolPermission permission) noexcept {
    switch (permission) {
    case ToolPermission::ReadConversations: return "read conversations";
    case ToolPermission::RunGeneration: return "run generation";
    case ToolPermission::WriteFiles: return "write files";
    case ToolPermission::Network: return "network";
    }
    return "unknown";
}

/// Adds a tool declaration or replaces a matching declaration and disables it again.
void IdleToolRegistry::declare(IdleToolDeclaration declaration) {
    declaration.enabled = false;
    const auto match = findByName(m_declarations, declaration.name);
    if (match == m_declarations.end()) {
        m_declarations.push_back(std::move(declaration));
        return;
    }
    // Replaced rather than merged, and switched off again. Whatever the user
    // agreed to was the previous declaration, and a declaration that has
    // changed what a tool does is not the thing they agreed to -- including
    // when the new version asks for more than the old one.
    *match = std::move(declaration);
}

/// Returns the named declaration, or null if no such tool is registered.
const IdleToolDeclaration* IdleToolRegistry::find(std::string_view name) const {
    const auto match = std::find_if(
        m_declarations.begin(), m_declarations.end(),
        [name](const IdleToolDeclaration& declaration) { return declaration.name == name; });
    return match == m_declarations.end() ? nullptr : &*match;
}

/// Returns a copy of all registered tool declarations and their enabled states.
std::vector<IdleToolDeclaration> IdleToolRegistry::tools() const {
    return m_declarations;
}

/// Changes a registered tool's enabled state; undeclared names are ignored.
void IdleToolRegistry::setEnabled(std::string_view name, bool enabled) {
    // An undeclared tool is not an error. The interface lists what is declared,
    // and a request about something that is not there has nothing to act on;
    // `enabled` answers false for it, which is the whole of the report.
    const auto match = findByName(m_declarations, name);
    if (match != m_declarations.end()) {
        match->enabled = enabled;
    }
}

/// Returns whether the named tool is both declared and enabled.
bool IdleToolRegistry::enabled(std::string_view name) const {
    const IdleToolDeclaration* declaration = find(name);
    return declaration != nullptr && declaration->enabled;
}

/// Adds or removes a granted capability without duplicating grants.
void IdleToolRegistry::grant(ToolPermission permission, bool granted) {
    const auto match = std::find(m_granted.begin(), m_granted.end(), permission);
    if (granted) {
        if (match == m_granted.end()) {
            m_granted.push_back(permission);
        }
        return;
    }
    if (match != m_granted.end()) {
        m_granted.erase(match);
    }
}

/// Returns whether the capability has been explicitly granted.
bool IdleToolRegistry::granted(ToolPermission permission) const {
    return holds(m_granted, permission);
}

/// Requires the tool to be declared, enabled, and granted every declared permission.
bool IdleToolRegistry::permits(std::string_view name) const {
    const IdleToolDeclaration* declaration = find(name);
    // Three separate reasons to refuse, and all three are checked: a tool that
    // was never declared, a tool the user has not switched on, and a tool whose
    // declared capabilities are not all granted. Collapsing the last two would
    // let a tool that asks for nothing run unasked, which is the opposite of
    // what an opt-in registry is for.
    return declaration != nullptr && declaration->enabled
        && missing(name).empty();
}

/// Returns outstanding permissions; undeclared tools report every known capability missing.
std::vector<ToolPermission> IdleToolRegistry::missing(std::string_view name) const {
    const IdleToolDeclaration* declaration = find(name);
    if (declaration == nullptr) {
        // Nothing is known about it, so everything it would need is missing.
        // Returning an empty list would read as "nothing is outstanding", and
        // that is the one answer this function must never give.
        return {kAllPermissions[0], kAllPermissions[1], kAllPermissions[2], kAllPermissions[3]};
    }

    std::vector<ToolPermission> outstanding;
    for (const ToolPermission permission : declaration->permissions) {
        if (!holds(m_granted, permission)) {
            outstanding.push_back(permission);
        }
    }
    return outstanding;
}

/// Declares the opt-in topic indexer with conversation-read permission.
IdleToolDeclaration indexThreadsDeclaration() {
    IdleToolDeclaration declaration;
    declaration.name = std::string(kIndexThreadsTool);
    declaration.summary = "Read this session's messages and note what it has been about.";
    declaration.permissions = {ToolPermission::ReadConversations};
    // Opt-in, like every tool. Even this one asks first: it is the user's
    // conversation, and deciding that Kestrel may read it unprompted is theirs.
    declaration.enabled = false;
    return declaration;
}

/// Declares the opt-in summarizer with conversation-read and generation permissions.
IdleToolDeclaration summariseSessionDeclaration() {
    IdleToolDeclaration declaration;
    declaration.name = std::string(kSummariseSessionTool);
    declaration.summary = "Ask the model to write down what this session was about.";
    // ReadConversations to assemble the material, RunGeneration because it
    // genuinely spends tokens. Both are declared, so both have to be granted:
    // the cost is the user's to agree to, and it is the sort of thing that
    // should never happen quietly on a background timer.
    declaration.permissions = {ToolPermission::ReadConversations,
                               ToolPermission::RunGeneration};
    declaration.enabled = false;
    return declaration;
}

namespace {

// Words that carry no topic. Kept short on purpose: a longer list starts
// deleting words that genuinely are the subject, and an index that quietly
// omits what the user was talking about is worse than one that shows a little
// noise.
bool isNoise(std::string_view word) {
    static const char* const kNoise[] = {
        "the", "and", "for", "with", "that", "this", "you", "your", "are", "was",
        "have", "has", "had", "but", "not", "can", "will", "would", "could",
        "should", "from", "into", "about", "what", "when", "where", "which",
        "there", "their", "them", "then", "than", "just", "like", "some", "more",
        "also", "been", "were", "its", "it's", "our", "out", "over", "only",
    };
    for (const char* noise : kNoise) {
        if (word == noise) {
            return true;
        }
    }
    return word.size() < 4;
}

/// Returns a lowercase copy using unsigned bytes for character classification.
std::string lowercase(std::string_view text) {
    std::string result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(static_cast<char>(
            std::tolower(static_cast<unsigned char>(character))));
    }
    return result;
}

// Splits on anything that is not a letter or a digit, so punctuation and the
// punctuation people actually type both end a word rather than joining two.
std::vector<std::string> words(std::string_view text) {
    std::vector<std::string> result;
    std::string current;
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0) {
            current.push_back(character);
            continue;
        }
        if (!current.empty()) {
            result.push_back(current);
            current.clear();
        }
    }
    if (!current.empty()) {
        result.push_back(current);
    }
    return result;
}

/// Counts distinct user-message terms and summarizes the three most frequent topics.
ToolRunResult indexThreads(const std::vector<Message>& messages) {
    ToolRunResult result;
    std::map<std::string, std::size_t> counts;
    for (const Message& message : messages) {
        // Only what the user said is worth indexing. The assistant's own
        // replies largely repeat the request, and counting them would make
        // everything the user asked twice look like the most important thing
        // in the session.
        if (message.role != MessageRole::User) {
            continue;
        }
        for (const std::string& word : words(lowercase(message.content))) {
            if (!isNoise(word)) {
                ++counts[word];
            }
        }
    }

    std::vector<std::pair<std::string, std::size_t>> ranked(counts.begin(), counts.end());
    std::sort(ranked.begin(), ranked.end(),
              [](const auto& left, const auto& right) {
                  if (left.second != right.second) {
                      return left.second > right.second;
                  }
                  return left.first < right.first;
              });

    constexpr std::size_t kMaxTopics = 3;
    std::string topics;
    for (std::size_t i = 0; i < ranked.size() && i < kMaxTopics; ++i) {
        if (!topics.empty()) {
            topics += ", ";
        }
        topics += ranked[i].first;
    }

    result.indexed = counts.size();
    if (topics.empty()) {
        result.summary = "Nothing worth indexing yet.";
        return result;
    }
    result.summary = "Indexed " + std::to_string(result.indexed) + " distinct terms. Leading topics: "
                     + topics + ".";
    return result;
}

} // namespace

/// Checks declaration, enablement, and permissions before indexing; reports refusals in the result.
ToolRunResult runIdleTool(const IdleToolRegistry& registry, std::string_view name,
                          const std::vector<Message>& messages) {
    ToolRunResult result;
    const IdleToolDeclaration* declaration = registry.find(name);
    if (declaration == nullptr) {
        result.summary = "No idle tool named '" + std::string(name) + "' is declared.";
        return result;
    }
    if (!registry.enabled(name)) {
        result.summary = "'" + declaration->name + "' is switched off.";
        return result;
    }

    // The check the tool cannot get past. It is repeated here rather than
    // trusted to the caller, because a tool that runs without it is exactly the
    // thing the registry exists to make impossible.
    const std::vector<ToolPermission> outstanding = registry.missing(name);
    if (!outstanding.empty()) {
        std::string needed;
        for (const ToolPermission permission : outstanding) {
            if (!needed.empty()) {
                needed += ", ";
            }
            needed += toString(permission);
        }
        result.summary = "'" + declaration->name + "' still needs permission to " + needed + ".";
        return result;
    }

    if (name == kIndexThreadsTool) {
        result = indexThreads(messages);
        result.ran = true;
        return result;
    }

    if (name == kSummariseSessionTool) {
        // Authorize the asynchronous generation; the app owns the worker.
        result.ran = true;
        result.summary = "Session summary authorized.";
        return result;
    }

    // Declared but not implemented. A registry entry is a promise, and the
    // honest way to keep it is to say which promise is outstanding.
    result.summary = "'" + declaration->name + "' is declared but not implemented yet.";
    return result;
}

} // namespace kestrel::core
