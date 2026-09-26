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
    // Backend-reported throughput. The app measures its own live figure from
    // delivered tokens, so this is advisory rather than authoritative.
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
//   * Callbacks are invoked with the backend's internal lock still held, so a
//     callback must not call back into the same backend (status(),
//     countTokens(), loadModel()). That re-enters a non-recursive mutex, which
//     is undefined behaviour rather than a slow path. Hand the work to another
//     thread and inspect the result there.
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

    // Counts tokens in `text` using this backend's tokenizer.
    //
    // A backend that has loaded a real model must override this so context
    // accounting is exact. The default is a documented approximation, which
    // keeps a usable number flowing for backends with no tokenizer while
    // making it obvious in the source that the figure is not exact rather
    // than letting a caller assume it is.
    [[nodiscard]] virtual std::size_t countTokens(std::string_view text) const;

    // Clears accumulated per-session accounting, e.g. the running context
    // occupancy. Called when the conversation is cleared or switched.
    virtual void resetContextUsage();

    // Declares the shared instruction prefix for a conversation.
    //
    // This text is identical on every turn, so a backend that can keep it
    // resident decodes it once and reuses those KV entries instead of
    // reprocessing it on every request. That is the single largest avoidable
    // cost in a chat turn: the system prompt is often longer than the reply.
    //
    // Callers pass generate() the per-turn prompt only. The prefix is the
    // backend's business, and a backend that caches it must not expect to see
    // it again in the request.
    //
    // Safe to call from the UI thread: an implementation defers the work to the
    // next generate() call, because the model context is only safe to touch
    // from the worker thread.
    virtual void setSystemPrompt(std::string_view text);

    // The prefix currently declared, for display and for tests.
    [[nodiscard]] virtual std::string_view systemPrompt() const noexcept;

    // Forgets the prefix and everything derived from it. A new conversation
    // must call this, or the next turn inherits the previous one's prompt.
    virtual void clearSharedPrefix();

    // Prefix tokens the backend is actually keeping resident, which is the
    // figure the diagnostics panel reports. Zero means the prefix is not
    // cached and is being resent with every request.
    [[nodiscard]] virtual std::size_t cachedPrefixTokens() const;

protected:
    // Shared by every backend so the prefix text has one owner. Subclasses
    // that cache the prefix decode it separately.
    std::string m_systemPrompt;
};

} // namespace kestrel::runtime
