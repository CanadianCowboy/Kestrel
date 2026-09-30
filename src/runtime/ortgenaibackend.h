#pragma once

#include "runtime/modelbackend.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

namespace kestrel::runtime {

// ONNX Runtime GenAI backend.
//
// The Windows-native real-inference path, and the one that replaces the
// TensorRT stub this file's sibling used to be. It runs a GenAI-format model
// -- a directory holding genai_config.json, model.onnx and its external data --
// on the CUDA execution provider when the build has a toolkit and the machine
// has a device, and on the CPU provider otherwise.
//
// Why this and not TensorRT, stated once so it does not get relitigated:
//
//   * TensorRT-LLM, which is what NVIDIA directs LLM inference to, documents
//     Linux as its platform. This is a Windows application.
//   * TensorRT-RTX, the Windows route, is reached through ONNX Runtime's
//     execution-provider API rather than through nvonnxparser, so the existing
//     TensorRTBackend could not have consumed it without becoming this.
//   * ONNX Runtime GenAI is what powers Foundry Local, Windows ML and the VS
//     Code AI Toolkit, and is the supported way to run a local LLM in a
//     Windows app.
//
// The only translation unit that includes an ort_genai header. Everything else
// sees this class, so a build with no ONNX Runtime GenAI present still compiles
// and reports the backend unavailable rather than failing to link.
//
// Threading: identical to every other ModelBackend. generate() runs on a
// worker thread and is the only method that touches the generator, because a
// generator is not safe for concurrent use. loadModel() may be called from the
// UI thread, so the model pointer is guarded. cancel() is deliberately
// lock-free and only performs an atomic store.
class OrtGenAiBackend final : public ModelBackend {
public:
    // Constructs with no model loaded. Availability is settled here rather than
    // on the first load, for the same reason the llama.cpp backend does it: a
    // backend that reports itself unavailable until it has already loaded a
    // model is invisible to everything that asks first, and selectBackend()
    // skips it.
    OrtGenAiBackend();
    ~OrtGenAiBackend() override;

    OrtGenAiBackend(const OrtGenAiBackend&) = delete;
    OrtGenAiBackend& operator=(const OrtGenAiBackend&) = delete;

    [[nodiscard]] BackendKind kind() const noexcept override;
    [[nodiscard]] RuntimeStatus status() const override;

    // Loads the GenAI model rooted at `modelPath`, which is a directory rather
    // than a single file: a GenAI model is a config plus one or more ONNX
    // graphs plus external data, and the layout is what identifies it.
    bool loadModel(const std::string& modelPath, std::string& error) override;

    void generate(const GenerationRequest& request,
                  TokenCallback onToken,
                  CompletionCallback onComplete) override;
    void cancel() override;

    // The model's real tokenizer, so context accounting is exact rather than
    // the shared chars-per-token approximation.
    [[nodiscard]] std::size_t countTokens(std::string_view text) const override;
    void resetContextUsage() override;

    // The system prompt is prepended to each turn's prompt. It is not kept
    // resident between turns -- see the implementation for why, and for what
    // cachedPrefixTokens() consequently reports.
    void setSystemPrompt(std::string_view text) override;
    void clearSharedPrefix() override;
    [[nodiscard]] std::size_t cachedPrefixTokens() const override;

    // Overrides the context length the VRAM budget chose. Zero restores the
    // automatic choice. Exposed because a context ceiling derived from a
    // device's total memory is a guess about what will fit alongside
    // everything else on the card, and the guess is sometimes wrong.
    void setContextLengthForTesting(int tokens);

private:
    // Defined only when KESTREL_HAS_ORT_GENAI is set. Held through an opaque
    // wrapper so this header never mentions an Oga type.
    struct Impl;

    // Recomputes m_status from the current model/generator state.
    // Caller must hold m_mutex.
    void refreshStatus();

    mutable std::mutex m_mutex;
    std::unique_ptr<Impl> m_impl;
    std::atomic<bool> m_cancelled{false};
    RuntimeStatus m_status;
    std::string m_modelPath;
    // The context actually in force, which is the smaller of what the model
    // declares and what this machine's VRAM can hold.
    int m_contextLength = 0;
    // Bytes the KV cache occupies at m_contextLength, and the share in use.
    std::size_t m_kvCacheBytes = 0;
    std::size_t m_kvCacheBytesUsed = 0;
    std::size_t m_contextUsed = 0;
    // Set when an explicit context length was requested and the budget did not
    // choose it, so the status can say which number won and why.
    int m_requestedContextLength = 0;
};

} // namespace kestrel::runtime
