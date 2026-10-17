#pragma once

#include "runtime/modelbackend.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>

class QNetworkAccessManager;

namespace kestrel::app {

// Qt Network stays in this desktop adapter, never in the portable runtime.
// The manager is created on the generation thread, including in tests.
class GroqBackend final : public runtime::ModelBackend {
public:
    using NetworkFactory = std::function<std::unique_ptr<QNetworkAccessManager>()>;
    explicit GroqBackend(std::string apiKey, NetworkFactory networkFactory = {});

    [[nodiscard]] runtime::BackendKind kind() const noexcept override;
    [[nodiscard]] runtime::RuntimeStatus status() const override;
    bool loadModel(const std::string& model, std::string& error) override;
    void generate(const runtime::GenerationRequest& request, runtime::TokenCallback onToken,
                  runtime::CompletionCallback onComplete) override;
    void cancel() override;
    void setSystemPrompt(std::string_view text) override;
    void clearSharedPrefix() override;
    void resetContextUsage() override;
    [[nodiscard]] std::size_t cachedPrefixTokens() const override;

private:
    std::string m_apiKey;
    NetworkFactory m_networkFactory;
    mutable std::mutex m_mutex;
    runtime::RuntimeStatus m_status;
    std::atomic<bool> m_cancelled{false};
};

} // namespace kestrel::app
