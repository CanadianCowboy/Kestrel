#include "runtime/llamacppbackend.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <vector>

#ifdef KESTREL_HAS_LLAMA_CPP
#include <llama.h>
#endif

namespace kestrel::runtime {

#ifdef KESTREL_HAS_LLAMA_CPP

// llama_backend_init() is process-global and reference counted upstream, but
// calling it once per backend instance is cheap and keeps the pairing obvious.
namespace {

void ensureBackendInitialised() {
    static const bool initialised = [] {
        llama_backend_init();
        return true;
    }();
    (void)initialised;
}

} // namespace

struct LlamaCppBackend::Impl {
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    std::string modelPath;
};

LlamaCppBackend::LlamaCppBackend() {
    ensureBackendInitialised();
    m_impl = std::make_unique<Impl>();
    m_status.detail = "llama.cpp linked; no model loaded";
}

LlamaCppBackend::~LlamaCppBackend() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_impl) {
        if (m_impl->context != nullptr) {
            llama_free(m_impl->context);
            m_impl->context = nullptr;
        }
        if (m_impl->model != nullptr) {
            llama_model_free(m_impl->model);
            m_impl->model = nullptr;
        }
    }
}

BackendKind LlamaCppBackend::kind() const noexcept {
    return BackendKind::LlamaCpp;
}

RuntimeStatus LlamaCppBackend::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

bool LlamaCppBackend::loadModel(const std::string& modelPath, std::string& error) {
    std::unique_lock<std::mutex> lock(m_mutex);

    if (!m_impl || m_impl->model == nullptr) {
        error = "llama.cpp is not linked in this build";
        return false;
    }

    // Release the previous pair before loading a replacement, so a failed load
    // does not leave a half-swapped model and context behind.
    if (m_impl->context != nullptr) {
        llama_free(m_impl->context);
        m_impl->context = nullptr;
    }
    if (m_impl->model != nullptr) {
        llama_model_free(m_impl->model);
        m_impl->model = nullptr;
    }

    llama_model_params modelParams = llama_model_default_params();
    // n_gpu_layers is left at the library default so a CUDA-enabled llama.cpp
    // build offloads automatically; Kestrel's own CUDA probe reports whether
    // a GPU is actually present.
    llama_model* model = llama_model_load_from_file(modelPath.c_str(), modelParams);
    if (model == nullptr) {
        error = "llama.cpp could not load " + modelPath +
                ". Is it a GGUF file produced for this build?";
        refreshStatusLocked(lock);
        return false;
    }

    llama_context_params contextParams = llama_context_default_params();
    // The training context is not a sensible default for chat; size the KV
    // cache to something a desktop agent can actually hold.
    contextParams.n_ctx = 4096;

    llama_context* context = llama_init_from_model(model, contextParams);
    if (context == nullptr) {
        llama_model_free(model);
        error = "llama.cpp allocated a model but could not create a context. "
                "Check available RAM and the requested context size.";
        refreshStatusLocked(lock);
        return false;
    }

    m_impl->model = model;
    m_impl->context = context;
    m_impl->modelPath = modelPath;
    m_contextUsed = 0;
    refreshStatusLocked(lock);
    return true;
}

void LlamaCppBackend::refreshStatusLocked(std::unique_lock<std::mutex>&) {
    m_status.modelLoaded = m_impl != nullptr && m_impl->model != nullptr;
    if (m_status.modelLoaded) {
        const std::filesystem::path path(m_impl->modelPath);
        m_status.modelName =
            path.has_filename() ? path.filename().string() : m_impl->modelPath;
        m_status.contextLimit = llama_n_ctx(m_impl->model);
        m_status.contextUsed = m_contextUsed;
        m_status.detail = "Loaded " + m_status.modelName;
    } else {
        m_status.modelName = "No model loaded";
        m_status.contextLimit = 0;
        m_status.contextUsed = 0;
    }
    // Availability means "this backend can serve requests at all", which is
    // true as soon as the library is linked. The registry falls back to the
    // mock when no model is loaded, via modelLoaded.
    m_status.available = true;
}

std::size_t LlamaCppBackend::countTokens(std::string_view text) const {
    if (text.empty()) {
        return 0;
    }
    const std::size_t exact = countTokensImpl(text);
    if (exact != 0) {
        return exact;
    }
    // No model loaded: fall back rather than reporting a cost of zero.
    return ModelBackend::countTokens(text);
}

std::size_t LlamaCppBackend::countTokensImpl(std::string_view text) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_impl == nullptr || m_impl->model == nullptr) {
        return 0;
    }

    // Ask the library how many tokens it needs, then tokenize for real. The
    // two-call pattern avoids guessing an upper bound that silently truncates
    // a long prompt.
    const int needed = llama_tokenize(
        m_impl->model, text.data(), static_cast<int>(text.size()), nullptr, 0, true, true);
    if (needed <= 0) {
        return 0;
    }
    std::vector<llama_token> tokens(static_cast<std::size_t>(needed));
    const int written = llama_tokenize(m_impl->model, text.data(),
                                       static_cast<int>(text.size()), tokens.data(), needed,
                                       true, true);
    return written > 0 ? static_cast<std::size_t>(written) : 0;
}

void LlamaCppBackend::resetContextUsage() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_contextUsed = 0;
    std::unique_lock<std::mutex> adopt = std::adopt_lock;
    refreshStatusLocked(adopt);
}

void LlamaCppBackend::generate(const GenerationRequest& request,
                               TokenCallback onToken,
                               CompletionCallback onComplete) {
    m_cancelled.store(false, std::memory_order_release);

    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_impl == nullptr || m_impl->model == nullptr || m_impl->context == nullptr) {
        onComplete(false, "No llama.cpp model is loaded");
        return;
    }

    llama_model* model = m_impl->model;
    llama_context* context = m_impl->context;
    const llama_vocab* vocab = llama_model_get_vocab(model);

    // Tokenize the prompt. Ask for the size first so a long prompt is never
    // silently truncated against a guessed buffer.
    const std::string& prompt = request.prompt;
    const int promptTokens = llama_tokenize(model, prompt.data(),
                                            static_cast<int>(prompt.size()), nullptr, 0,
                                            true, true);
    if (promptTokens <= 0) {
        onComplete(false, "llama.cpp could not tokenize the prompt");
        return;
    }

    std::vector<llama_token> tokens;
    tokens.reserve(static_cast<std::size_t>(promptTokens) + request.maxTokens + 8);
    tokens.resize(static_cast<std::size_t>(promptTokens));
    if (llama_tokenize(model, prompt.data(), static_cast<int>(prompt.size()), tokens.data(),
                       promptTokens, true, true) != promptTokens) {
        onComplete(false, "llama.cpp tokenized the prompt inconsistently");
        return;
    }

    const std::size_t promptSize = tokens.size();

    // Sampler chain: temperature for generation, greedy only as a fallback.
    // Composition order matters, so the chain is built once per request.
    llama_sampler_chain_params chainParams = llama_sampler_chain_default_params();
    chainParams.no_perf = true;
    llama_sampler* chain = llama_sampler_chain_init(chainParams);
    llama_sampler_chain_add(chain, llama_sampler_init_penalties(
                                       /* penalty_last_n */ 64, /* penalty_repeat */ 1.0F,
                                       /* penalty_freq */ 0.0F, /* penalty_present */ 0.0F));
    if (request.temperature <= 0.0F) {
        llama_sampler_chain_add(chain, llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(chain, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(chain, llama_sampler_init_temp(request.temperature));
        llama_sampler_chain_add(chain, llama_sampler_init_dist(1234));
    }

    // Decode the prompt in one batch, then sample one token at a time.
    llama_batch batch = llama_batch_get_one(tokens.data(), static_cast<int32_t>(promptSize));
    if (llama_decode(context, batch) != 0) {
        llama_sampler_free(chain);
        onComplete(false, "llama.cpp failed to decode the prompt");
        return;
    }

    const int contextLimit = static_cast<int>(llama_n_ctx(model));
    const int maxTokens =
        request.maxTokens > 0 ? request.maxTokens : contextLimit;
    std::string piece;
    std::size_t produced = 0;
    bool sawStop = false;

    for (int i = 0; i < maxTokens; ++i) {
        // Cooperative cancellation. Checked before every decode so a stop
        // request takes effect within one token rather than at batch end.
        if (m_cancelled.load(std::memory_order_acquire)) {
            sawStop = true;
            break;
        }
        // Leave headroom so the next token always has a slot; a full context
        // would make llama_decode fail and look like a model error.
        if (static_cast<int>(tokens.size()) >= contextLimit - 1) {
            break;
        }

        const llama_token next =
            llama_sampler_sample(chain, context, static_cast<int32_t>(tokens.size()) - 1);
        if (llama_vocab_is_eog(vocab, next)) {
            break;
        }

        tokens.push_back(next);
        ++produced;

        char buffer[256];
        const int length = llama_token_to_piece(model, next, buffer, sizeof(buffer), 0, true);
        if (length > 0) {
            piece.assign(buffer, static_cast<std::size_t>(length));
            onToken(piece);
        }

        batch = llama_batch_get_one(&tokens[tokens.size() - 1], 1);
        if (llama_decode(context, batch) != 0) {
            llama_sampler_free(chain);
            onComplete(false, "llama.cpp failed while decoding a generated token");
            return;
        }
    }

    llama_sampler_free(chain);

    // Context grows by the prompt plus what this turn produced, capped at the
    // model's real window so the UI cannot show an impossible fill level.
    m_contextUsed = std::min<std::size_t>(
        static_cast<std::size_t>(contextLimit),
        m_contextUsed + promptSize + produced);
    std::unique_lock<std::mutex> adopt = std::adopt_lock;
    refreshStatusLocked(adopt);

    if (sawStop) {
        onComplete(false, "Generation stopped");
    } else {
        onComplete(true, {});
    }
}

void LlamaCppBackend::cancel() {
    // Lock-free on purpose: called from the UI thread while the worker is
    // inside generate() holding m_mutex, so taking the lock here could
    // deadlock against a path that waits on this flag.
    m_cancelled.store(true, std::memory_order_release);
}

#else // !KESTREL_HAS_LLAMA_CPP

struct LlamaCppBackend::Impl {};

LlamaCppBackend::LlamaCppBackend() = default;
LlamaCppBackend::~LlamaCppBackend() = default;

BackendKind LlamaCppBackend::kind() const noexcept {
    return BackendKind::LlamaCpp;
}

RuntimeStatus LlamaCppBackend::status() const {
    return m_status;
}

bool LlamaCppBackend::loadModel(const std::string&, std::string& error) {
    error = "llama.cpp is not linked in this build. Configure with "
            "-DKESTREL_ENABLE_LLAMA_CPP=ON -DKESTREL_LLAMA_CPP_ROOT=<path> and rebuild.";
    return false;
}

void LlamaCppBackend::generate(const GenerationRequest&, TokenCallback,
                               CompletionCallback onComplete) {
    onComplete(false, "llama.cpp is not linked in this build");
}

std::size_t LlamaCppBackend::countTokens(std::string_view text) const {
    return ModelBackend::countTokens(text);
}

void LlamaCppBackend::resetContextUsage() {
    m_contextUsed = 0;
}

void LlamaCppBackend::refreshStatusLocked(std::unique_lock<std::mutex>&) {}

std::size_t LlamaCppBackend::countTokensImpl(std::string_view) const {
    return 0;
}

void LlamaCppBackend::cancel() {
    m_cancelled.store(true, std::memory_order_release);
}

#endif // KESTREL_HAS_LLAMA_CPP

} // namespace kestrel::runtime
