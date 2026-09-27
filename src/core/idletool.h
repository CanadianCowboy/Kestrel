#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/conversation.h"

namespace kestrel::core {

// The capabilities an idle tool can possibly ask for.
//
// The set is closed and declared, which is the point: a tool declares what it
// needs, the user grants capabilities, and the registry refuses anything that
// does not add up. Nothing here is inferred from a tool's behaviour, and a tool
// cannot reach a capability that is not in this list -- the same rule the idle
// task kinds follow, applied to the one place where real work happens.
enum class ToolPermission {
    ReadConversations, // look at what has been said in this session
    RunGeneration,     // spend tokens on the model
    WriteFiles,        // create or modify something on disk
    Network,           // talk to anything outside this machine
};

[[nodiscard]] const char* toString(ToolPermission permission) noexcept;

// What a tool says it needs and what the user is told it does.
struct IdleToolDeclaration {
    std::string name;
    std::string summary;
    std::vector<ToolPermission> permissions;
    // False unless a tool opts in. A tool that touches the conversation or the
    // model has to be asked for, even though it is local and harmless, because
    // "local and harmless" is a judgement the user should make rather than one
    // the code makes on their behalf.
    bool enabledByDefault = false;
};

// The registry of tools the idle loop may ask for.
//
// It is the whole permission story, and it is deliberately boring: a request is
// allowed only when the tool is declared, the tool is switched on, and every
// permission it declared has been granted. Anything else is refused and the
// reason is available to the caller so the interface can say what is missing
// rather than silently doing nothing.
class IdleToolRegistry {
public:
    // Declares a tool, replacing any earlier declaration with the same name. A
    // replacement does not inherit the previous tool's enabled state: a tool
    // that has just changed what it does is a tool the user has not agreed to.
    void declare(IdleToolDeclaration declaration);

    [[nodiscard]] const IdleToolDeclaration* find(std::string_view name) const;
    [[nodiscard]] std::vector<IdleToolDeclaration> tools() const;

    void setEnabled(std::string_view name, bool enabled);
    [[nodiscard]] bool enabled(std::string_view name) const;

    // Grants a capability. Nothing is granted by default, not even the ones that
    // sound harmless.
    void grant(ToolPermission permission, bool granted);
    [[nodiscard]] bool granted(ToolPermission permission) const;

    // True only when the tool is declared, enabled, and fully granted.
    [[nodiscard]] bool permits(std::string_view name) const;
    // The permissions a request for `name` is still missing, for the interface.
    [[nodiscard]] std::vector<ToolPermission> missing(std::string_view name) const;

private:
    std::vector<IdleToolDeclaration> m_declarations;
    // Granted capabilities, as a vector of the same enum. A bitmask would be
    // tidier and would grow a conversion function the first time a permission is
    // added; this stays obvious and stays small.
    std::vector<ToolPermission> m_granted;
};

// What one run of a tool produced.
//
// `ran` is false when the registry refused, and `summary` then says why. Keeping
// the refusal in the same shape as the success means the caller has one thing to
// put in front of the user rather than a silent skip and a separate error path.
struct ToolRunResult {
    bool ran = false;
    std::string summary;
    std::string detail;
    std::size_t indexed = 0;
};

// The one tool that ships, and the name it is declared under.
//
// It reads the conversation and summarises what it has been about. It writes
// nothing and reaches nothing outside the process, so it asks for one
// capability: ReadConversations. A tool that did not need the model should not
// declare RunGeneration just because the registry has a slot for it -- a
// permission that is declared is a permission the user has to reason about, and
// granting one that is not used is trust the user gave away for nothing.
inline constexpr std::string_view kIndexThreadsTool = "index recent threads";

// A tool that asks the model to summarise the session. It declares RunGeneration
// because it genuinely spends tokens, and that is the whole point of declaring
// it: the user is agreeing to a cost, not just to a read.
inline constexpr std::string_view kSummariseSessionTool = "summarise the session";

[[nodiscard]] IdleToolDeclaration indexThreadsDeclaration();
[[nodiscard]] IdleToolDeclaration summariseSessionDeclaration();

// Runs the named tool. Refuses, rather than runs, unless the registry permits
// it -- the check lives here and not at the call site so that no caller can
// forget it.
[[nodiscard]] ToolRunResult runIdleTool(const IdleToolRegistry& registry,
                                        std::string_view name,
                                        const std::vector<Message>& messages);

} // namespace kestrel::core
