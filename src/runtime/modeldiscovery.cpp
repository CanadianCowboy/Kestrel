#include "runtime/modeldiscovery.h"

#include "core/pathtext.h"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace kestrel::runtime {
namespace {

bool hasExtension(const std::filesystem::path& path, std::string_view expected) {
    std::string extension = core::pathText(path.extension());
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == expected;
}

std::size_t directoryBytes(const std::filesystem::path& root) {
    std::size_t total = 0;
    std::error_code error;
    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, error), end;
    while (!error && it != end) {
        std::error_code entryError;
        if (it->is_regular_file(entryError)) {
            const auto bytes = it->file_size(entryError);
            if (!entryError) {
                total += static_cast<std::size_t>(bytes);
            }
        }
        it.increment(error);
    }
    return total;
}

bool genAiModelLooksLoadable(const std::filesystem::path& root) {
    std::error_code error;
    const auto config = root / "genai_config.json";
    if (!std::filesystem::is_regular_file(config, error)) {
        return false;
    }
    const auto bytes = std::filesystem::file_size(config, error);
    if (error || bytes == 0 || !std::ifstream(config, std::ios::binary).good()) {
        return false;
    }
    std::filesystem::directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, error), end;
    while (!error && it != end) {
        std::error_code entryError;
        if (it->is_regular_file(entryError) && hasExtension(it->path(), ".onnx")) {
            const auto graphBytes = it->file_size(entryError);
            if (!entryError && graphBytes > 0) {
                return true;
            }
        }
        it.increment(error);
    }
    return false;
}

} // namespace

std::vector<ModelCandidate> discoverModels(const std::filesystem::path& searchDirectory) {
    std::vector<ModelCandidate> ranked;
    std::error_code error;
    std::filesystem::directory_iterator it(
        searchDirectory, std::filesystem::directory_options::skip_permission_denied, error), end;
    while (!error && it != end) {
        const auto path = it->path();
        std::error_code entryError;
        if (it->is_directory(entryError)) {
            if (genAiModelLooksLoadable(path)) {
                ranked.push_back({core::pathText(path), directoryBytes(path)});
            }
        } else if (it->is_regular_file(entryError) && hasExtension(path, ".gguf")) {
            const auto bytes = it->file_size(entryError);
            if (!entryError && bytes > 0 && std::ifstream(path, std::ios::binary).good()) {
                ranked.push_back({core::pathText(path), static_cast<std::size_t>(bytes)});
            }
        }
        it.increment(error);
    }
    std::sort(ranked.begin(), ranked.end(), [](const ModelCandidate& a, const ModelCandidate& b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.path < b.path;
    });
    return ranked;
}

} // namespace kestrel::runtime
