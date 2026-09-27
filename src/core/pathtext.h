#pragma once

#include <filesystem>
#include <string>
#include <string_view>

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

// The other direction: a path handed to this project as UTF-8 text.
//
// Qt speaks UTF-8, so every path that arrives from the interface -- a model
// chosen in a file dialog, a directory named by an environment variable -- is
// a std::string holding UTF-8. On Windows, std::filesystem::path's constructor
// from std::string reads those bytes as the active ANSI code page instead, so a
// model in a folder whose name has a Cyrillic or CJK character in it does not
// exist as far as every is_directory and file_size call is concerned. The
// failure is a plain "no such file or directory" pointing at a path that is
// plainly there.
//
// std::filesystem::u8path() says this correctly and is deprecated in C++20, so
// this is the same conversion spelled out: a char8_t view of exactly the same
// bytes, which the path constructor accepts directly. Whether the bytes really
// are UTF-8 is the caller's claim, not something to guess at or to assert.
[[nodiscard]] std::filesystem::path pathFromUtf8(std::string_view text);

} // namespace kestrel::core
