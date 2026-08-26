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
