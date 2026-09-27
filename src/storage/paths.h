#pragma once

#include <filesystem>

namespace kestrel::storage {

// Where Kestrel keeps the files it owns.
//
// One place, asked once, because the three platforms disagree about where an
// application may write: %LOCALAPPDATA% on Windows, ~/Library/Application
// Support on macOS, and $XDG_DATA_HOME on everything else. Hardcoding one of
// them is how a project ends up writing into a home directory that a sync
// client then uploads.
//
// Returns an empty path only if the environment is unusable, which callers
// must treat as "do not persist" rather than "write somewhere else".
[[nodiscard]] std::filesystem::path userDataDirectory();

// The subdirectory holding sealed blobs. Kept apart from the profile document
// so that a user who deletes the profile by hand can see what remains, and so
// the two can be backed up or cleared independently.
[[nodiscard]] std::filesystem::path sealedBlobDirectory();

} // namespace kestrel::storage
