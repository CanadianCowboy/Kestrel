#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace kestrel::runtime {

enum class BackendKind {
    Mock,
    LlamaCpp,
    TensorRT,
};

struct GenerationRequest {
    std::string prompt;
    float temperature = 0.7F;
    int maxTokens = 512;
};

struct RuntimeStatus {
    bool available = false;
    bool modelLoaded = false;
    std::string backendName;
    std::string modelName;
    std::string detail;
    double tokensPerSecond = 0.0;
    std::size_t contextUsed = 0;
    std::size_t contextLimit = 0;
};

using TokenCallback = std::function<void(std::string_view token)>;
using CompletionCallback = std::function<void(bool success, std::string_view error)>;

// Threading contract for every ModelBackend implementation:
//
//   * generate() blocks until the response finishes or is cancelled. It is
//     always called from a worker thread, never from the UI thread.
//   * The onToken and onComplete callbacks run on that same worker thread.
//     Callers marshal them back to the UI thread themselves.
//   * cancel() is the only method that may be called from the UI thread
//     while generate() is running on a worker. Implementations must make it
//     safe under that concurrency: an atomic flag, or a driver call the
//     backend's own API permits from another thread. A plain bool is a data
//     race, not a working implementation.
//   * Cancellation is cooperative. generate() must poll its flag between
//     tokens so that cancel() takes effect promptly; a backend parked in one
//     long GPU call cannot be interrupted until that call returns.
class ModelBackend {
public:
    virtual ~ModelBackend() = default;

    [[nodiscard]] virtual BackendKind kind() const noexcept = 0;
    [[nodiscard]] virtual RuntimeStatus status() const = 0;
    virtual bool loadModel(const std::string& modelPath, std::string& error) = 0;
    virtual void generate(const GenerationRequest& request,
                          TokenCallback onToken,
                          CompletionCallback onComplete) = 0;
    virtual void cancel() = 0;
};

} // namespace kestrel::runtime
