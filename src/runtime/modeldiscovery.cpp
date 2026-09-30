#include "runtime/modeldiscovery.h"

#include <algorithm>
#include <fstream>

namespace kestrel::runtime {

namespace {

// Total bytes of a candidate, which for a GenAI model is the whole directory:
// essentially all of the weight is in the external data file beside the graph,
// so summing the folder is summing the model.
std::size_t directoryBytes(const std::filesystem::path& root,
                           std::error_code& error) {
    std::size_t total = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, error)) {
        if (entry.is_regular_file(error)) {
            total += static_cast<std::size_t>(entry.file_size(error));
        }
    }
    return total;
}

// A GenAI model is identified by its config, and a config that is empty or
// unreadable describes no model however many gigabytes sit beside it. The
// directory is not a candidate until the config and a graph are both there, so
// the caller's first guess is one worth making.
bool genAiModelLooksLoadable(const std::filesystem::path& root) {
    std::error_code error;
    const auto config = root / "genai_config.json";
    if (!std::filesystem::is_regular_file(config, error)) {
        return false;
    }
    if (std::filesystem::file_size(config, error) != 0 || error) {
        return false;
    }
    // file_size says the bytes are there; opening it says we may read them. The
    // standard filesystem library has no readability query, and a directory
    // full of gigabytes behind a config nobody can open is not a model.
    std::ifstream readable(config, std::ios::binary);
    if (!readable.good()) {
        return false;
    }
    // Without a graph there is nothing to run, whatever else the folder holds.
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
        if (entry.is_regular_file(error) && entry.path().extension() == ".onnx") {
            return true;
        }
    }
    return false;
}

} // namespace

std::vector<ModelCandidate> discoverModels(const std::filesystem::path& searchDirectory) {
    std::vector<ModelCandidate> ranked;
    std::error_code error;
    if (!std::filesystem::is_directory(searchDirectory, error)) {
        return ranked;
    }

    for (const auto& entry : std::filesystem::directory_iterator(searchDirectory, error)) {
        if (!entry.is_regular_file(error) && !entry.is_directory(error)) {
            continue;
        }
        const std::filesystem::path path = entry.path();
        if (entry.is_directory(error)) {
            if (genAiModelLooksLoadable(path)) {
                ranked.push_back({path.string(), directoryBytes(path, error)});
            }
            continue;
        }
        if (path.extension() != ".gguf") {
            continue;
        }
        const auto bytes = std::filesystem::file_size(path, error);
        if (error || bytes == 0) {
            // Zero bytes is a download that has not started, and a reader will
            // either refuse it or fail on it later. Not a candidate.
            continue;
        }
        ranked.push_back({path.string(), static_cast<std::size_t>(bytes)});
    }

    std::sort(ranked.begin(), ranked.end(),
              [](const ModelCandidate& left, const ModelCandidate& right) {
                  if (left.bytes != right.bytes) {
                      return left.bytes > right.bytes;
                  }
                  return left.path < right.path;
              });
    return ranked;
}

} // namespace kestrel::runtime
