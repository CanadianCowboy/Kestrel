#pragma once

#include "runtime/modelbackend.h"

namespace kestrel::runtime {

class LlamaCppBackend final : public ModelBackend {
public:
    [[nodiscard]] BackendKind kind() const noexcept override;
    [[nodiscard]] RuntimeStatus status() const override;
    bool loadModel(const std::string& modelPath, std::string& error) override;
    void generate(const GenerationRequest& request,
                  TokenCallback onToken,
                  CompletionCallback onComplete) override;
    void cancel() override;

private:
    RuntimeStatus m_status{
        false,
        false,
        "llama.cpp",
        {},
        "Build with llama.cpp/CUDA to enable this backend",
        0.0,
        0,
        0,
    };
};

} // namespace kestrel::runtime
