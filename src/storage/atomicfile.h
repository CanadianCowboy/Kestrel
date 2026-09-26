#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace kestrel::storage {

// Writes a file so that a reader ever sees either the old contents or the new
// ones, and never a mixture.
//
// The store is written on every change and read on every start, so the window
// between "truncated" and "renamed" is exactly the window in which a crash --
// or a full disk, or a user closing the laptop -- would leave a profile that
// no longer parses. The user would come back to an empty assistant with no
// way to tell that from a fresh install.
//
// So the bytes go to a temporary file in the *same directory* (a rename across
// filesystems is not atomic, and a temp directory is not always on the same
// volume), are flushed to the device, and only then replace the target. On
// Windows the replacement uses MoveFileEx with MOVEFILE_REPLACE_EXISTING and
// MOVEFILE_WRITE_THROUGH, because ReplaceFile and a plain rename both have
// their own ways of leaving a gap.
[[nodiscard]] bool writeFileAtomically(const std::filesystem::path& target,
                                       const std::vector<std::uint8_t>& bytes, std::string& error);

// The text form, which is what every caller actually has.
[[nodiscard]] bool writeFileAtomically(const std::filesystem::path& target, std::string_view text,
                                       std::string& error);

// Reads a whole file, or reports why not. A missing file is not an error here;
// the caller decides what an absent profile means.
[[nodiscard]] bool readWholeFile(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes,
                                 std::string& error);

} // namespace kestrel::storage
