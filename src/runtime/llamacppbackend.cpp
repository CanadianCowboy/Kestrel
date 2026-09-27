#include "runtime/llamacppbackend.h"

#include "core/pathtext.h"
#include "runtime/cudadevice.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#ifdef KESTREL_HAS_LLAMA_CPP
#include <llama.h>
#endif

namespace kestrel::runtime {

#ifdef KESTREL_HAS_LLAMA_CPP

namespace {

// llama_backend_init() is process-global. Doing it once here keeps the pairing
// with the model lifetime obvious.
void ensureBackendInitialised() {
    static const bool initialised = [] {
        llama_backend_init();
        return true;
    }();
    (void)initialised;
}

// Tokenizes `text` into `out`, returning the number of tokens written or 0.
//
// llama_tokenize reports "this text needs N tokens" with a *negative* return
// value when the buffer it was given is too small to hold them, and its header
// documents the probe call (null buffer, capacity 0) as the way to learn N in
// advance. That probe overflows by construction, so it always comes back
// negative. Reading the negative as a failure is what made this backend report
// that it could not tokenize any prompt at all, and therefore generate nothing.
int tokenizeInto(const llama_vocab* vocab,
                 std::string_view text,
                 std::vector<llama_token>& out,
                 bool addSpecial = true) {
    const char* data = text.data();
    const auto length = static_cast<int32_t>(text.size());

    int capacity = llama_tokenize(vocab, data, length, nullptr, 0,
                                  /* add_special */ addSpecial, /* parse_special */ true);
    if (capacity < 0) {
        capacity = -capacity;
    }
    if (capacity == 0) {
        return 0;
    }

    out.resize(static_cast<std::size_t>(capacity));
    const int written = llama_tokenize(vocab, data, length, out.data(), capacity,
                                       /* add_special */ addSpecial, /* parse_special */ true);
    if (written < 0) {
        return 0;
    }
    out.resize(static_cast<std::size_t>(written));
    return written;
}

// Bytes of KV cache one token occupies for `model`, or zero when the model's
// shape is not something the arithmetic can be trusted for.
//
// One K row and one V row per layer per cached token, each
// (embedding / heads) * kvHeads elements. Under grouped-query attention those
// rows are far narrower than the model's embedding, which is the only reason a
// long context is affordable at all on a card this size.
std::size_t kvBytesPerTokenFor(const llama_model* model) {
    const int32_t layers = llama_model_n_layer(model);
    const int32_t heads = llama_model_n_head(model);
    const int32_t headsKv = llama_model_n_head_kv(model);
    const int32_t embd = llama_model_n_embd(model);
    if (layers <= 0 || heads <= 0 || headsKv <= 0 || embd <= 0 || embd % heads != 0) {
        // Without a clean head dimension any figure here would be invented.
        return 0;
    }
    const auto rowElements = static_cast<int64_t>(embd / heads) * headsKv;
    return static_cast<std::size_t>(layers) * (ggml_row_size(GGML_TYPE_F16, rowElements)
                                              + ggml_row_size(GGML_TYPE_F16, rowElements));
}

// How much of the device's memory the KV cache may take, once the weights are
// resident.
//
// Two thirds, after a fixed 512 MB is set aside. The shortfall is not slack for
// its own sake: the CUDA context itself costs a few hundred megabytes before a
// single byte of cache exists, and a cache sized to exactly the remaining bytes
// does not fail politely -- it fails at context creation, with an
// out-of-memory error that names no setting the user can change.
constexpr double kKvBudgetShare = 0.66;
constexpr std::size_t kFixedOverheadBytes = 512ULL * 1024ULL * 1024ULL;

// The context to create for `model` on this machine.
//
// Three numbers, in order of authority: what the model was trained for, what
// this machine can hold, and a floor for a machine that can be asked neither.
int chooseContextLength(const llama_model* model) {
    const int trained = llama_n_ctx_train(model);
    const int ceiling = trained > 0 ? trained : 8192;

    const std::size_t perToken = kvBytesPerTokenFor(model);
    if (perToken == 0) {
        return std::min(ceiling, 8192);
    }

    const CudaProbe probe = probeCuda();
    const CudaDeviceInfo* device = probe.selectedDevice();
    if (device == nullptr || device->totalMemoryBytes == 0) {
        // No GPU to budget against. System RAM is the next honest thing to size
        // against, and llama.cpp will place the KV there, but it is not known
        // cheaply and a wrong guess is expensive; 8192 is safe everywhere.
        return std::min(ceiling, 8192);
    }

    const std::size_t total = device->totalMemoryBytes;
    // The weights are already resident by the time this runs, so the honest
    // budget is what is left. Approximated from the model's own file rather
    // than re-read, because the file is megabytes to gigabytes and the load has
    // just been through it.
    const std::size_t weights = static_cast<std::size_t>(
        std::max<int64_t>(llama_model_size(model), 0));
    const std::size_t spare = total > weights + kFixedOverheadBytes
                                  ? total - weights - kFixedOverheadBytes
                                  : 0;
    const auto affordable = static_cast<std::size_t>(
        static_cast<double>(spare) * kKvBudgetShare) / perToken;
    if (affordable < 1024) {
        // The model does not fit with a usable context. 1024 is still a context
        // -- enough for a system prompt and a short reply -- and the caller
        // will see a small window reported rather than a load that failed for a
        // reason nobody could act on.
        return 1024;
    }
    // Rounded down to a multiple of 1024 so the reported figure reads as a
    // deliberate choice rather than the residue of a division.
    const int budgeted = static_cast<int>((affordable / 1024) * 1024);
    return std::min(ceiling, budgeted);
}

} // namespace

struct LlamaCppBackend::Impl {
    llama_model* model = nullptr;
    llama_context* context = nullptr;
    std::string modelPath;

    // The KV element types the context was created with. Kept because llama.cpp
    // exposes no accessor for the resolved types, and the KV byte total is
    // meaningless without them.
    ggml_type typeK = GGML_TYPE_F16;
    ggml_type typeV = GGML_TYPE_F16;

    // The shared prefix, tokenized once. Its KV entries stay in the context
    // between turns; prefixDirty records whether the context still matches
    // this text, since only generate() can safely reconcile the two.
    std::vector<llama_token> prefixTokens;
    bool prefixDirty = true;
};

// Initialises the process-global llama.cpp library once and starts this
// backend with empty model/context state.
LlamaCppBackend::LlamaCppBackend() {
    ensureBackendInitialised();
    m_impl = std::make_unique<Impl>();
    m_status.detail = "llama.cpp linked; no model loaded";
    // Resolve the status now rather than on the first load. A backend that
    // reports itself unavailable until it has already loaded a model is
    // invisible to everything that asks first: selectBackend() skipped it, the
    // UI hid the model picker, and loading a file was refused with a message
    // claiming no model was loaded. Reaching this translation unit at all
    // means llama.cpp is linked, so availability is settled here.
    refreshStatus();
}

// Frees any loaded context and model before this backend is destroyed.
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

// Identifies this as the llama.cpp backend.
BackendKind LlamaCppBackend::kind() const noexcept {
    return BackendKind::LlamaCpp;
}

// Returns a snapshot of the current status under lock.
RuntimeStatus LlamaCppBackend::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

/// Releases the previous model/context and loads a GGUF with a context sized to
/// the machine. Returns false with an error on failure; serializes access with m_mutex.
bool LlamaCppBackend::loadModel(const std::string& modelPath, std::string& error) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // No "is the library linked" guard here. Reaching this translation unit at
    // all means llama.cpp is linked; that is a compile-time fact, not runtime
    // state. A previous version tested m_impl->model != nullptr, which is null
    // before the *first* load, so the very first loadModel call was rejected
    // as though the library were missing.

    // Release the previous pair before loading a replacement, so a failed load
    // cannot leave a half-swapped model and context behind.
    if (m_impl->context != nullptr) {
        llama_free(m_impl->context);
        m_impl->context = nullptr;
    }
    if (m_impl->model != nullptr) {
        llama_model_free(m_impl->model);
        m_impl->model = nullptr;
    }
    m_impl->modelPath.clear();
    refreshStatus();

    llama_model_params modelParams = llama_model_default_params();
    // n_gpu_layers is left at the library default so a CUDA-enabled llama.cpp
    // build offloads automatically; Kestrel's own probe reports whether a GPU
    // is actually present.
    llama_model* model = llama_model_load_from_file(modelPath.c_str(), modelParams);
    if (model == nullptr) {
        error = "llama.cpp could not load " + modelPath +
                ". Is it a GGUF file produced for this build?";
        refreshStatus();
        return false;
    }

    llama_context_params contextParams = llama_context_default_params();
    // The context used to be a literal 4096 with the comment "the training
    // context is not a sensible default for chat". True as far as it went, and
    // it left the question of what *is* sensible unanswered -- so every model
    // got the same 4K window regardless of the machine, and a conversation
    // longer than that silently lost its oldest turns. Qwen3.5 declares
    // 262144; the useful range on real hardware is set by VRAM, not by the
    // model card, so the number is derived from the device rather than chosen.
    //
    // Fallback when there is no device to reason about: 8192, which holds a
    // system prompt and a long conversation on any machine Kestrel runs on and
    // is still small enough for an 8 GB card with a 4B model resident.
    const int contextLength = chooseContextLength(model);
    contextParams.n_ctx = contextLength;

    llama_context* context = llama_init_from_model(model, contextParams);
    if (context == nullptr) {
        llama_model_free(model);
        error = "llama.cpp allocated a model but could not create a context. "
                "Check available RAM and the requested context size.";
        refreshStatus();
        return false;
    }
    m_impl->model = model;
    m_impl->context = context;
    m_impl->modelPath = modelPath;
    m_impl->typeK = contextParams.type_k;
    m_impl->typeV = contextParams.type_v;
    // A new context holds none of the old model's KV entries, so the prefix
    // has to be decoded again against it.
    m_impl->prefixTokens.clear();
    m_impl->prefixDirty = true;
    m_contextUsed = 0;
    refreshStatus();
    return true;
}

/// Updates model identity, context occupancy, and estimated KV bytes.
/// The caller must hold m_mutex.
void LlamaCppBackend::refreshStatus() {
    const bool loaded = m_impl != nullptr && m_impl->model != nullptr && m_impl->context != nullptr;
    m_status.modelLoaded = loaded;
    if (loaded) {
        // u8path is deprecated in C++20 and the path constructor takes the same
        // u8string. Built from an explicit conversion rather than a silent
        // reinterpret: the source is a std::string, and whether its bytes are
        // UTF-8 is the caller's claim, not something to assert here.
        const std::filesystem::path path(m_impl->modelPath);
        // core::pathText rather than filename().string(): a model file under a
        // user name the ANSI code page cannot spell throws from string() on
        // Windows, and this is the line that decides what the UI calls the model.
        m_status.modelName = path.has_filename() ? core::pathText(path.filename()) : m_impl->modelPath;
        // The context window is a property of the context, not the model, so
        // this reads the live allocation rather than a training default.
        m_status.contextLimit = llama_n_ctx(m_impl->context);
        m_status.contextUsed = m_contextUsed;
        const std::size_t total = kvCacheBytes();
        m_status.kvCacheBytes = total;
        // The used share is the filled part of the same allocation, so it
        // tracks the token count exactly rather than estimating separately.
        m_status.kvCacheBytesUsed =
            (total > 0 && m_status.contextLimit > 0)
                ? total * m_contextUsed / m_status.contextLimit
                : 0;
        m_status.detail = "Loaded " + m_status.modelName;
    } else {
        m_status.modelName = "No model loaded";
        m_status.contextLimit = 0;
        m_status.contextUsed = 0;
        m_status.kvCacheBytes = 0;
        m_status.kvCacheBytesUsed = 0;
    }
    // Availability means "this backend can serve requests at all", which is
    // true as soon as the library is linked. modelLoaded is what the registry
    // consults to decide whether a model is actually usable.
    m_status.available = true;
}

// Returns the token count for `text`, using the real tokenizer when a model
// is loaded and falling back to the shared approximation otherwise.
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

// Tokenizes `text` with the loaded model's vocabulary, or returns 0 if no
// model is loaded.
std::size_t LlamaCppBackend::countTokensImpl(std::string_view text) const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_impl == nullptr || m_impl->model == nullptr) {
        return 0;
    }
    std::vector<llama_token> tokens;
    return static_cast<std::size_t>(
        tokenizeInto(llama_model_get_vocab(m_impl->model), text, tokens));
}

// Clears the tracked context usage back to zero.
void LlamaCppBackend::resetContextUsage() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_contextUsed = 0;
    refreshStatus();
}

/// Stores changed prefix text under m_mutex and invalidates its token cache.
/// Defers context updates to generate(); identical text leaves the cache intact.
void LlamaCppBackend::setSystemPrompt(std::string_view text) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_systemPrompt == text) {
        // Unchanged: leave the resident KV entries alone. Redecoding an
        // identical prefix every turn would cost exactly what caching saves.
        return;
    }
    m_systemPrompt = text;
    if (m_impl != nullptr) {
        m_impl->prefixTokens.clear();
        m_impl->prefixDirty = true;
    }
}

/// Declares an empty prefix, deferring any context invalidation to the next generation.
void LlamaCppBackend::clearSharedPrefix() {
    setSystemPrompt({});
}

/// Estimates full-context KV bytes from model dimensions and configured K/V types.
/// Returns zero for missing or invalid dimensions; the caller must hold m_mutex.
std::size_t LlamaCppBackend::kvCacheBytes() const {
    if (m_impl == nullptr || m_impl->model == nullptr || m_impl->context == nullptr) {
        return 0;
    }
    const int32_t layers = llama_model_n_layer(m_impl->model);
    const int32_t heads = llama_model_n_head(m_impl->model);
    const int32_t headsKv = llama_model_n_head_kv(m_impl->model);
    const int32_t embd = llama_model_n_embd(m_impl->model);
    if (layers <= 0 || heads <= 0 || headsKv <= 0 || embd <= 0
        || embd % heads != 0) {
        // Without a clean head dimension any figure here would be invented.
        return 0;
    }

    // One K row and one V row per layer per cached token. Under grouped-query
    // attention the rows are narrower than the model's embedding, which is
    // exactly why a KV cache is much smaller than the weights it serves.
    const int64_t rowElements = static_cast<int64_t>(embd / heads) * headsKv;
    const auto cells = static_cast<std::size_t>(llama_n_ctx(m_impl->context));
    if (cells == 0) {
        return 0;
    }
    const std::size_t rowK = ggml_row_size(m_impl->typeK, rowElements);
    const std::size_t rowV = ggml_row_size(m_impl->typeV, rowElements);
    if (rowK == 0 || rowV == 0) {
        return 0;
    }
    return static_cast<std::size_t>(layers) * cells * (rowK + rowV);
}

/// Returns the resident prefix length under m_mutex, or zero while it is dirty or absent.
std::size_t LlamaCppBackend::cachedPrefixTokens() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_impl == nullptr) {
        return 0;
    }
    // While dirty the prefix is not resident, so reporting the token count of
    // its text would claim a saving that has not happened yet.
    return m_impl->prefixDirty ? 0 : m_impl->prefixTokens.size();
}

/// Returns the retained prefix length, or zero if unavailable; the caller must hold m_mutex.
std::size_t LlamaCppBackend::applySystemPrefix(bool& prefixFailed) {
    prefixFailed = false;
    if (m_impl == nullptr || m_impl->context == nullptr) {
        return 0;
    }
    llama_memory_t memory = llama_get_memory(m_impl->context);

    if (m_impl->prefixDirty) {
        // The prefix changed (or this is the first turn): nothing in the cache
        // can be trusted, so start from empty and decode the prefix once.
        llama_memory_clear(memory, /* data */ true);
        m_impl->prefixTokens.clear();
        if (!m_systemPrompt.empty()) {
            tokenizeInto(llama_model_get_vocab(m_impl->model), m_systemPrompt, m_impl->prefixTokens);
            const auto prefixSize = static_cast<int32_t>(m_impl->prefixTokens.size());
            if (prefixSize > 0) {
                llama_batch batch = llama_batch_init(prefixSize, 0, 1);
                batch.n_tokens = prefixSize;
                for (int32_t i = 0; i < prefixSize; ++i) {
                    batch.token[i] = m_impl->prefixTokens[static_cast<std::size_t>(i)];
                    batch.pos[i] = static_cast<llama_pos>(i);
                    // n_seq_id is a per-token count, not a batch-wide flag, and
                    // llama_batch_init leaves it uninitialised. Setting only the
                    // first entry makes the library read a garbage count for
                    // every later token and reject the batch.
                    batch.n_seq_id[i] = 1;
                    batch.seq_id[i][0] = 0;
                    // No logits wanted: the turn's own last token supplies them.
                    batch.logits[i] = 0;
                }
                const int rc = llama_decode(m_impl->context, batch);
                llama_batch_free(batch);
                if (rc != 0) {
                    // A partially written prefix would leave the model reading
                    // a cache that does not match the text, so drop the partial
                    // entries and report the failure rather than continuing.
                    m_impl->prefixTokens.clear();
                    llama_memory_clear(memory, /* data */ true);
                    m_impl->prefixDirty = false;
                    prefixFailed = true;
                }
            }
        }
        m_impl->prefixDirty = false;
    } else if (!m_impl->prefixTokens.empty()) {
        // The prefix is unchanged, so keep its KV entries and drop only what
        // the previous turn appended after them.
        llama_memory_seq_rm(memory, 0, static_cast<llama_pos>(m_impl->prefixTokens.size()), -1);
    }
    return m_impl->prefixTokens.size();
}

/// Decodes the shared prefix and per-turn prompt, then streams sampled tokens until done.
/// Calls onComplete for success, cancellation, or failure while holding m_mutex; callbacks must not re-enter.
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

    // Bring the context in line with the declared system prompt. On the first
    // turn this decodes the prefix; on every turn after it the prefix's KV
    // entries are already resident and are reused rather than recomputed.
    bool prefixFailed = false;
    const std::size_t prefixLength = applySystemPrefix(prefixFailed);
    if (prefixFailed) {
        onComplete(false, "llama.cpp could not decode the system prompt into the context");
        return;
    }

    // Tokenize only this turn's prompt. The prefix is already in the context,
    // so including it here would both redo the work and double-count it.
    std::vector<llama_token> tokens;
    if (tokenizeInto(vocab, request.prompt, tokens, /* addSpecial */ prefixLength == 0) <= 0) {
        onComplete(false, "llama.cpp could not tokenize the prompt");
        return;
    }
    const std::size_t promptSize = tokens.size();
    // Room for the continuation, so the vector does not reallocate per token.
    tokens.reserve(promptSize + static_cast<std::size_t>(request.maxTokens > 0 ? request.maxTokens : 256));

    // Sampler chain. Composition order matters, so it is built once per request.
    llama_sampler_chain_params chainParams = llama_sampler_chain_default_params();
    chainParams.no_perf = true;
    llama_sampler* chain = llama_sampler_chain_init(chainParams);
    // The penalty sampler needs the vocabulary size as its first argument.
    llama_sampler_chain_add(chain,
                            llama_sampler_init_penalties(llama_vocab_n_tokens(vocab),
                                                        /* penalty_last_n */ 64,
                                                        /* penalty_repeat */ 1.0F,
                                                        /* penalty_freq */ 0.0F,
                                                        /* penalty_present */ 0.0F));
    if (request.temperature <= 0.0F) {
        llama_sampler_chain_add(chain, llama_sampler_init_greedy());
    } else {
        llama_sampler_chain_add(chain, llama_sampler_init_top_k(40));
        llama_sampler_chain_add(chain, llama_sampler_init_temp(request.temperature));
        llama_sampler_chain_add(chain, llama_sampler_init_dist(1234));
    }

    // Decode this turn's tokens at the positions that follow the prefix.
    // Positions are explicit rather than delegated to llama_batch_get_one,
    // which always numbers a batch from 0 and would collide with the prefix.
    const auto turnSize = static_cast<int32_t>(promptSize);
    llama_batch promptBatch = llama_batch_init(turnSize, 0, 1);
    promptBatch.n_tokens = turnSize;
    for (int32_t i = 0; i < turnSize; ++i) {
        promptBatch.token[i] = tokens[static_cast<std::size_t>(i)];
        promptBatch.pos[i] = static_cast<llama_pos>(prefixLength + static_cast<std::size_t>(i));
        // Per-token count, not a batch-wide flag: llama_batch_init leaves the
        // array uninitialised and the library reads one entry per token.
        promptBatch.n_seq_id[i] = 1;
        promptBatch.seq_id[i][0] = 0;
        // Only the final token's logits are needed, to sample the reply from.
        promptBatch.logits[i] = (i == turnSize - 1) ? 1 : 0;
    }
    const int promptRc = llama_decode(context, promptBatch);
    llama_batch_free(promptBatch);
    if (promptRc != 0) {
        llama_sampler_free(chain);
        onComplete(false, "llama.cpp failed to decode the prompt");
        return;
    }

    const auto contextLimit = static_cast<int>(llama_n_ctx(context));
    const int maxTokens = request.maxTokens > 0 ? request.maxTokens : contextLimit;

    // Continuation tokens need explicit positions. llama_batch_get_one always
    // numbers a batch from 0, so using it for a one-token continuation would
    // tell the model every generated token sat at position 0, corrupting the
    // KV cache and producing degenerate output.
    llama_batch step = llama_batch_init(1, 0, 1);
    std::string piece;
    std::size_t produced = 0;
    bool sawStop = false;

    for (int i = 0; i < maxTokens; ++i) {
        // Cooperative cancellation, checked before every decode so a stop
        // request takes effect within one token rather than at batch end.
        if (m_cancelled.load(std::memory_order_acquire)) {
            sawStop = true;
            break;
        }
        // Leave headroom so the next token always has a slot. Decoding into a
        // full context fails, and that would surface as a model error rather
        // than a truncated response. The prefix counts against the window
        // because it occupies real positions in the cache.
        if (contextLimit > 0 && static_cast<int>(prefixLength + tokens.size()) >= contextLimit - 1) {
            break;
        }

        // -1 means "sample from the logits of the most recent decode", which is
        // the documented idiom and avoids depending on an index into the batch.
        const llama_token next = llama_sampler_sample(chain, context, -1);
        // Feed the token back so stateful samplers (penalties, DRY) can see the
        // history they are meant to penalise.
        llama_sampler_accept(chain, next);


        if (llama_vocab_is_eog(vocab, next)) {
            break;
        }

        tokens.push_back(next);
        ++produced;

        char buffer[256];
        const int length = llama_token_to_piece(vocab, next, buffer,
                                                static_cast<int32_t>(sizeof(buffer)),
                                                /* lstrip */ 0, /* special */ true);
        if (length > 0) {
            piece.assign(buffer, static_cast<std::size_t>(length));
            onToken(piece);
        }

        // One token, at its true position in the sequence. n_seq_id is a
        // per-token count that llama_batch_init leaves uninitialised, and
        // seq_id/logits are uninitialised too, so every member the next decode
        // reads is set explicitly.
        step.n_tokens = 1;
        step.token[0] = next;
        step.pos[0] = static_cast<llama_pos>(prefixLength + tokens.size() - 1);
        step.n_seq_id[0] = 1;
        step.seq_id[0][0] = 0;
        step.logits[0] = 1;
        if (llama_decode(context, step) != 0) {
            llama_batch_free(step);
            llama_sampler_free(chain);
            onComplete(false, "llama.cpp failed while decoding a generated token");
            return;
        }
    }

    llama_batch_free(step);
    llama_sampler_free(chain);

    // Context usage is the resident prefix plus this turn's prompt and output,
    // capped at the model's real window so the UI cannot show an impossible
    // fill level.
    if (contextLimit > 0) {
        m_contextUsed = std::min<std::size_t>(
            static_cast<std::size_t>(contextLimit),
            prefixLength + promptSize + produced);
    }
    refreshStatus();

    if (sawStop) {
        onComplete(false, "Generation stopped");
    } else {
        onComplete(true, {});
    }
}

void LlamaCppBackend::cancel() {
    // Lock-free on purpose: called from the UI thread while the worker is
    // inside generate() holding m_mutex, so taking the lock here would
    // deadlock against a path that waits on this flag.
    m_cancelled.store(true, std::memory_order_release);
}

#else // !KESTREL_HAS_LLAMA_CPP

struct LlamaCppBackend::Impl {};

// No llama.cpp state to initialise or release when the library isn't linked.
LlamaCppBackend::LlamaCppBackend() = default;
LlamaCppBackend::~LlamaCppBackend() = default;

// Identifies this as the llama.cpp backend, even though it is unavailable.
BackendKind LlamaCppBackend::kind() const noexcept {
    return BackendKind::LlamaCpp;
}

// Returns the fixed "unavailable" status for a build without llama.cpp.
RuntimeStatus LlamaCppBackend::status() const {
    return m_status;
}

// Always fails: llama.cpp is not linked in this build.
bool LlamaCppBackend::loadModel(const std::string&, std::string& error) {
    error = "llama.cpp is not linked in this build. Configure with "
            "-DKESTREL_ENABLE_LLAMA_CPP=ON -DKESTREL_LLAMA_CPP_ROOT=<path> and rebuild.";
    return false;
}

// Always fails: llama.cpp is not linked in this build.
void LlamaCppBackend::generate(const GenerationRequest&, TokenCallback,
                               CompletionCallback onComplete) {
    onComplete(false, "llama.cpp is not linked in this build");
}

// No tokenizer is available, so this falls back to the shared approximation.
std::size_t LlamaCppBackend::countTokens(std::string_view text) const {
    return ModelBackend::countTokens(text);
}

// Clears the tracked context usage back to zero.
void LlamaCppBackend::resetContextUsage() {
    m_contextUsed = 0;
}

/// Stores prefix text for the build without llama.cpp; no cache is allocated.
void LlamaCppBackend::setSystemPrompt(std::string_view text) {
    m_systemPrompt = text;
}

/// Clears the stored prefix text in the build without llama.cpp.
void LlamaCppBackend::clearSharedPrefix() {
    m_systemPrompt.clear();
}

/// Returns zero because the build without llama.cpp has no resident prefix.
std::size_t LlamaCppBackend::cachedPrefixTokens() const {
    return 0;
}

/// Returns zero because the build without llama.cpp allocates no KV cache.
std::size_t LlamaCppBackend::kvCacheBytes() const {
    return 0;
}

/// Returns zero without modifying state because no llama.cpp context exists.
std::size_t LlamaCppBackend::applySystemPrefix(bool& prefixFailed) {
    prefixFailed = false;
    return 0;
}

// No live model/context state to refresh in this stub.
void LlamaCppBackend::refreshStatus() {}

// Always 0: no tokenizer is available without llama.cpp.
std::size_t LlamaCppBackend::countTokensImpl(std::string_view) const {
    return 0;
}

// Records a cancellation request; there is nothing running to stop.
void LlamaCppBackend::cancel() {
    m_cancelled.store(true, std::memory_order_release);
}

#endif // KESTREL_HAS_LLAMA_CPP

} // namespace kestrel::runtime
