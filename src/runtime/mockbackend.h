#pragma once

#include "runtime/modelbackend.h"

#include <atomic>
#include <cstddef>

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
    [[nodiscard]] std::size_t countTokens(std::string_view text) const override;
    void resetContextUsage() override;

private:
    // Written by cancel() on the UI thread while generate() polls it on the
    // worker thread, so this must be atomic rather than a plain bool.
    std::atomic<bool> m_cancelled{false};
    // Running context occupancy for the loaded "model". Maintained by the
    // backend because only the backend knows how its text is tokenized.
    std::size_t m_contextUsed = 0;
};

} // namespace kestrel::runtime
