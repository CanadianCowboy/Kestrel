#include "core/pathtext.h"

namespace kestrel::core {

std::string pathText(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.data()), text.size());
}

} // namespace kestrel::core
