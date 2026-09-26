#pragma once

#include "runtime/modelbackend.h"

#include <atomic>

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
    // Written by cancel() on the UI thread while generate() polls it on the
    // worker thread, so this must be atomic rather than a plain bool.
    std::atomic<bool> m_cancelled{false};
};

} // namespace kestrel::runtime
