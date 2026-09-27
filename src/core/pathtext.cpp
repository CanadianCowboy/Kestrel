#include "core/pathtext.h"

#include <cstring>

namespace kestrel::core {

std::string pathText(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

std::filesystem::path pathFromUtf8(std::string_view text) {
    // The same bytes, typed as UTF-8 rather than as whatever the active code
    // page happens to be. Length is carried through rather than relying on a
    // terminator, so an embedded NUL is preserved as part of the name instead
    // of silently truncating the path.
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(text.data()), text.size()));
}

} // namespace kestrel::core
