#include "storage/paths.h"

#include <cstdlib>
#include <string>

// getenv is the portable spelling and the one spelling that works on all three
// platforms. MSVC prefers _dupenv_s, which does not exist elsewhere, and the
// value read here is a directory name rather than anything secret.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif

namespace kestrel::storage {
namespace {

std::string environment(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr ? std::string(value) : std::string();
}

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace

std::filesystem::path userDataDirectory() {
    const std::filesystem::path root = [] {
#if defined(_WIN32)
        // LOCALAPPDATA rather than APPDATA: this is machine state, not
        // documents, and roaming it to another machine would take a profile
        // sealed with one account's key to a machine that cannot open it.
        const std::string base = environment("LOCALAPPDATA");
        if (base.empty()) {
            return std::filesystem::path();
        }
        return std::filesystem::path(base) / "Kestrel";
#elif defined(__APPLE__)
        const std::string home = environment("HOME");
        if (home.empty()) {
            return std::filesystem::path();
        }
        return std::filesystem::path(home) / "Library" / "Application Support" / "Kestrel";
#else
        std::string base = environment("XDG_DATA_HOME");
        if (base.empty()) {
            const std::string home = environment("HOME");
            if (home.empty()) {
                return std::filesystem::path();
            }
            base = home + "/.local/share";
        }
        return std::filesystem::path(base) / "kestrel";
#endif
    }();
    return root;
}

std::filesystem::path sealedBlobDirectory() {
    const std::filesystem::path root = userDataDirectory();
    if (root.empty()) {
        return {};
    }
    return root / "sealed";
}

} // namespace kestrel::storage
