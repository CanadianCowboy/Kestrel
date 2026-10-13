#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace kestrel::core {

// Reduces an HTML document to the text a reader would see: scripts, styles and
// comments are dropped, block elements become line breaks, common entities are
// decoded, and runs of whitespace are collapsed. The result is capped at
// `maxChars` bytes and says so when it is cut short.
//
// Deliberately not a browser. It does not run scripts, so a page that builds its
// text in JavaScript comes back nearly empty, and the caller should say that.
[[nodiscard]] std::string htmlToText(std::string_view html, std::size_t maxChars);

} // namespace kestrel::core
