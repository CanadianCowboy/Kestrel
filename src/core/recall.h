#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::core {

enum class MemoryKind { Fact, Summary, Episode };
[[nodiscard]] const char* toString(MemoryKind kind) noexcept;

struct Memory {
    std::uint64_t id = 0;
    MemoryKind kind = MemoryKind::Fact;
    std::string text;
    // Time is always supplied by the owner, never read here.
    std::string createdAt;
    std::string updatedAt;
    std::string source = "manual";
    bool sensitive = false;
    bool pinned = false;
    std::uint32_t confirmations = 1; // repeated user statements, not inferred truth
    std::uint64_t supersededBy = 0; // retained audit record, excluded from retrieval
};

// Conservative heuristics, not a guarantee that all private information is
// recognized. The UI states this explicitly; approved private records remain
// local-only regardless of retrieval relevance.
[[nodiscard]] bool memoryNeedsApproval(std::string_view text);
// Empty when the message is too large. Splits without cutting UTF-8 characters.
[[nodiscard]] std::vector<std::string> memoryChunks(std::string_view text);

class Recall {
public:
    static constexpr std::size_t kMaxTextLength = 500; // UTF-8 bytes, not characters.
    static constexpr std::size_t kMaxMemories = 2000;
    static constexpr std::size_t kMaxCaptureBytes = 2000;
    static constexpr std::size_t kMaxContextBytes = 2400;
    static constexpr std::size_t kMaxContextRecords = 8;

    // A bare domain object stays off until its owner attaches secure storage.
    // The desktop enables fresh profiles by default; explicit opt-outs survive.
    void setEnabled(bool enabled) noexcept;
    [[nodiscard]] bool enabled() const noexcept;

    // Exact ASCII-case/whitespace normalized duplicates reuse an existing id.
    // Sensitive material must have explicit approval even through this API.
    std::uint64_t remember(MemoryKind kind, std::string_view text, std::string_view createdAt,
                           std::string_view source = "manual", bool approvedSensitive = false);
    bool edit(std::uint64_t id, std::string_view text, std::string_view updatedAt,
              bool approvedSensitive = false);
    bool forget(std::uint64_t id);
    bool setPinned(std::uint64_t id, bool pinned);
    // Both records must be active; self-links/cycles are refused. The old record stays
    // inspectable but can no longer compete with its explicit correction.
    bool supersede(std::uint64_t oldId, std::uint64_t replacementId);
    void clear();
    void restore(bool enabled, std::vector<Memory> memories);
    [[nodiscard]] const std::vector<Memory>& memories() const noexcept;

    // Panel search is case-insensitive substring search, newest first.
    [[nodiscard]] std::vector<Memory> search(std::string_view query) const;
    // Lexical relevance with inverse document-frequency weighting; newest wins
    // ties. Nonmatching episodes never leak into unrelated questions. Explicit
    // facts are a small background profile. Sensitive records never leave local.
    [[nodiscard]] std::vector<Memory> retrieve(std::string_view query, bool cloud,
                                              std::size_t limit = kMaxContextRecords) const;
    // Formats only whole records that fit. selected receives the records really
    // included, so the UI does not claim a record was used when it was dropped.
    [[nodiscard]] std::string contextBlock(std::string_view query, bool cloud,
                                            std::size_t maxBytes,
                                            std::vector<Memory>* selected = nullptr) const;
    [[nodiscard]] std::string contextBlock(std::size_t maxBytes) const;

private:
    bool m_enabled = false;
    std::vector<Memory> m_memories;
    std::uint64_t m_nextId = 1;
};

} // namespace kestrel::core
