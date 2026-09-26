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

    /// The system prompt is decoded into the context once and then left
    /// resident, so each turn only pays to process its own tokens.
    void setSystemPrompt(std::string_view text) override;
    /// Clears the declared prefix; the next generation reconciles the context.
    void clearSharedPrefix() override;
    /// Returns resident prefix tokens, or zero when dirty, absent, or llama.cpp is unavailable.
    [[nodiscard]] std::size_t cachedPrefixTokens() const override;

private:
    // Defined only when KESTREL_HAS_LLAMA_CPP is set. Held through an opaque
    // wrapper so this header never mentions a llama type.
    struct Impl;

    // Caller must hold m_mutex.
    void refreshStatus();
    [[nodiscard]] std::size_t countTokensImpl(std::string_view text) const;

    /// Brings the context in line with the declared system prompt and returns
    /// the number of prefix tokens left resident.
    ///
    /// Called from generate() rather than from setSystemPrompt() because the
    /// context is not safe to touch from the UI thread. When the prefix has not
    /// changed, this drops only the tokens after it and leaves the prefix's KV
    /// entries in place, which is the whole point: the system prompt is not
    /// recomputed per turn.
    ///
    /// Caller must hold m_mutex.
    [[nodiscard]] std::size_t applySystemPrefix();

    /// Bytes of KV cache this model holds for a full context. Computed from the
    /// model's own shape and the KV types the context was created with, since
    /// llama.cpp exposes no accessor for the resolved allocation. Returns 0 when
    /// the model does not report enough to compute it honestly.
    [[nodiscard]] std::size_t kvCacheBytes() const;

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
