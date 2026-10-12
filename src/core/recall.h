#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::core {

// What kind of thing is remembered. A fact is something the user asked Kestrel
// to keep, or a preference it was told. A summary is a short account of a past
// conversation.
enum class MemoryKind {
    Fact,
    Summary,
};

[[nodiscard]] const char* toString(MemoryKind kind) noexcept;

struct Memory {
    std::uint64_t id = 0;
    MemoryKind kind = MemoryKind::Fact;
    std::string text;
    // Supplied by the owner. This layer never reads a clock.
    std::string createdAt;
};

// What Kestrel remembers across sessions, held in memory.
//
// Off by default. While off, remember() stores nothing. Turning recall off stops
// new storage and keeps what is already stored. Only clear() deletes.
//
// The model reads no files and no clock. The app loads it from the profile,
// saves it back, and supplies every timestamp.
class Recall {
public:
    // Longer than this is not a memory. A memory is a sentence or two, and a
    // long one is usually a pasted document that does not belong in recall.
    static constexpr std::size_t kMaxTextLength = 500;

    void setEnabled(bool enabled) noexcept;
    [[nodiscard]] bool enabled() const noexcept;

    // Stores one memory and returns its id. Returns 0, and stores nothing, when
    // recall is off, when the text is empty after trimming, or when it is longer
    // than kMaxTextLength.
    std::uint64_t remember(MemoryKind kind, std::string_view text, std::string_view createdAt);

    // Removes one memory. Returns false when no memory has that id.
    bool forget(std::uint64_t id);

    // Removes everything. The enabled flag is left as it is.
    void clear();

    // Replaces the contents with a loaded set. Ids are kept, so a memory keeps
    // the id it had. New memories continue after the largest id loaded.
    void restore(bool enabled, std::vector<Memory> memories);

    // Oldest first, in the order they were stored.
    [[nodiscard]] const std::vector<Memory>& memories() const noexcept;

    // Memories whose text contains the query, ignoring ASCII case, newest first.
    // An empty query matches everything.
    [[nodiscard]] std::vector<Memory> search(std::string_view query) const;

private:
    bool m_enabled = false;
    std::vector<Memory> m_memories;
    std::uint64_t m_nextId = 1;
};

} // namespace kestrel::core
