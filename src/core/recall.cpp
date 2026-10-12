#include "core/recall.h"

#include <algorithm>

namespace kestrel::core {

namespace {

std::string_view trim(std::string_view text) noexcept {
    const auto isBlank = [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!text.empty() && isBlank(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && isBlank(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

char foldAscii(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool containsIgnoringAsciiCase(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) {
        return true;
    }
    if (needle.size() > haystack.size()) {
        return false;
    }
    for (std::size_t start = 0; start + needle.size() <= haystack.size(); ++start) {
        bool matches = true;
        for (std::size_t k = 0; k < needle.size(); ++k) {
            if (foldAscii(haystack[start + k]) != foldAscii(needle[k])) {
                matches = false;
                break;
            }
        }
        if (matches) {
            return true;
        }
    }
    return false;
}

} // namespace

const char* toString(MemoryKind kind) noexcept {
    switch (kind) {
    case MemoryKind::Fact: return "fact";
    case MemoryKind::Summary: return "summary";
    }
    return "unknown";
}

void Recall::setEnabled(bool enabled) noexcept {
    m_enabled = enabled;
}

bool Recall::enabled() const noexcept {
    return m_enabled;
}

std::uint64_t Recall::remember(MemoryKind kind, std::string_view text, std::string_view createdAt) {
    if (!m_enabled) {
        return 0;
    }
    const std::string_view trimmed = trim(text);
    if (trimmed.empty() || trimmed.size() > kMaxTextLength) {
        return 0;
    }
    Memory memory;
    memory.id = m_nextId++;
    memory.kind = kind;
    memory.text = std::string(trimmed);
    memory.createdAt = std::string(createdAt);
    m_memories.push_back(memory);
    return memory.id;
}

bool Recall::forget(std::uint64_t id) {
    const auto found = std::find_if(m_memories.begin(), m_memories.end(),
                                    [id](const Memory& memory) { return memory.id == id; });
    if (found == m_memories.end()) {
        return false;
    }
    m_memories.erase(found);
    return true;
}

void Recall::clear() {
    m_memories.clear();
}

void Recall::restore(bool enabled, std::vector<Memory> memories) {
    m_enabled = enabled;
    m_memories = std::move(memories);
    std::uint64_t largest = 0;
    for (const Memory& memory : m_memories) {
        largest = std::max(largest, memory.id);
    }
    m_nextId = largest + 1;
}

const std::vector<Memory>& Recall::memories() const noexcept {
    return m_memories;
}

std::string Recall::contextBlock(std::size_t maxCharacters) const {
    if (!m_enabled || m_memories.empty()) {
        return {};
    }
    const std::string_view heading =
        "Things the user asked you to remember. Use them only where they bear on the "
        "question, and do not recite them:\n";
    std::string lines;
    for (auto memory = m_memories.rbegin(); memory != m_memories.rend(); ++memory) {
        const std::string line = "- " + memory->text + "\n";
        if (heading.size() + lines.size() + line.size() > maxCharacters) {
            break;
        }
        lines += line;
    }
    if (lines.empty()) {
        return {};
    }
    // The last line has no newline after it, so the block ends where its text does.
    lines.pop_back();
    return std::string(heading) + lines;
}

std::vector<Memory> Recall::search(std::string_view query) const {
    const std::string_view needle = trim(query);
    std::vector<Memory> matches;
    for (auto memory = m_memories.rbegin(); memory != m_memories.rend(); ++memory) {
        if (containsIgnoringAsciiCase(memory->text, needle)) {
            matches.push_back(*memory);
        }
    }
    return matches;
}

} // namespace kestrel::core
