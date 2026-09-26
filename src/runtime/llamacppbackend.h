#pragma once

#include "runtime/modelbackend.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace kestrel::runtime {

// llama.cpp GGUF backend.
//
// The only translation unit that includes a llama.h. Everything else sees this
// class, so a build with no llama.cpp present still compiles and reports the
// backend as unavailable rather than failing to link.
//
// Threading: generate() runs on a worker thread and is the only method that
// touches the llama_context, because a context is not safe for concurrent use.
// loadModel() may be called from the UI thread, so the model pointer is
// guarded; cancel() is deliberately lock-free and only performs an atomic
// store, matching the ModelBackend contract.
class LlamaCppBackend final : public ModelBackend {
public:
    LlamaCppBackend();
    ~LlamaCppBackend() override;

    LlamaCppBackend(const LlamaCppBackend&) = delete;
    LlamaCppBackend& operator=(const LlamaCppBackend&) = delete;

    [[nodiscard]] BackendKind kind() const noexcept override;
    [[nodiscard]] RuntimeStatus status() const override;
    bool loadModel(const std::string& modelPath, std::string& error) override;
    void generate(const GenerationRequest& request,
                  TokenCallback onToken,
                  CompletionCallback onComplete) override;
    void cancel() override;

    // Real tokenizer when a model is loaded. Falls back to the shared
    // approximation otherwise, so callers always get a usable number.
    [[nodiscard]] std::size_t countTokens(std::string_view text) const override;
    void resetContextUsage() override;

private:
    // Defined only when KESTREL_HAS_LLAMA_CPP is set. Held through an opaque
    // wrapper so this header never mentions a llama type.
    struct Impl;

    void refreshStatusLocked(std::unique_lock<std::mutex>& lock);
    [[nodiscard]] std::size_t countTokensImpl(std::string_view text) const;

    mutable std::mutex m_mutex;
    std::unique_ptr<Impl> m_impl;

    std::atomic<bool> m_cancelled{false};
    std::size_t m_contextUsed = 0;
    RuntimeStatus m_status{
        false,
        false,
        "llama.cpp",
        {},
        "Build with llama.cpp to enable this backend "
        "(configure with -DKESTREL_ENABLE_LLAMA_CPP=ON -DKESTREL_LLAMA_CPP_ROOT=<path>)",
        0.0,
        0,
        0,
    };
};

} // namespace kestrel::runtime
