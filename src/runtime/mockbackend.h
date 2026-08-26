#pragma once

#include "runtime/modelbackend.h"

namespace kestrel::runtime {

class MockBackend final : public ModelBackend {
public:
    [[nodiscard]] BackendKind kind() const noexcept override;
    [[nodiscard]] RuntimeStatus status() const override;
    bool loadModel(const std::string& modelPath, std::string& error) override;
    void generate(const GenerationRequest& request,
                  TokenCallback onToken,
                  CompletionCallback onComplete) override;
    void cancel() override;

private:
    bool m_cancelled = false;
};

} // namespace kestrel::runtime
