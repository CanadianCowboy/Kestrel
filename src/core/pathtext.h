#pragma once

#include <filesystem>
#include <string>

namespace kestrel::core {

// A path as text, for a message a person is going to read.
//
// Not path::string(). On Windows that converts the native wide path to the
// ANSI code page, and the MSVC STL throws std::system_error when a character
// has no representation there. Every path this project reports on reaches that
// call through a user name -- %LOCALAPPDATA% on Windows, $HOME elsewhere -- so
// a Cyrillic or CJK name on a Western system locale is enough to make it
// throw, and it throws out of functions whose only way to report a problem is
// the string they were handed. Where it does not throw it still quietly
// rewrites the name in whatever the active code page says the name is.
//
// This goes through UTF-8, which every name has a representation in. On POSIX
// the native encoding is already UTF-8 and this is a copy.
//
// One function, not one per file: three translation units needed this and the
// mistake being prevented is a silent one, so a second copy is a second chance
// to get it wrong.
[[nodiscard]] std::string pathText(const std::filesystem::path& path);

} // namespace kestrel::core
