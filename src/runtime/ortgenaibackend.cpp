#include "runtime/ortgenaibackend.h"

#include "core/pathtext.h"
#include "runtime/cudadevice.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef KESTREL_HAS_ORT_GENAI
#include <ort_genai.h>
#endif

namespace kestrel::runtime {

namespace {

// The context a machine gets when nothing can be derived: no VRAM to budget
// against, or a genai_config.json missing the fields the arithmetic needs.
// Chosen to be small enough to fit anywhere and large enough to hold a system
// prompt and a reply, which is the floor for the app to say anything useful.
constexpr int kFallbackContextLength = 4096;

// Share of the memory left after the weights that may go to the KV cache.
//
// Not 1.0, and the shortfall is not slack for its own sake. ONNX Runtime keeps
// its own CUDA arena, the CUDA context itself costs a few hundred megabytes,
// and the allocator does not return freed blocks promptly, so a cache sized to
// exactly the remaining bytes fails at load time rather than at some later
// point where the cause is obvious. Two thirds leaves room for all three.
constexpr double kKvBudgetShare = 0.66;

// Megabytes held back regardless of what the arithmetic says, for the same
// reasons as kKvBudgetShare. Subtracted before the share is applied so it is
// not scaled away on a machine with a lot of memory to spare.
constexpr std::size_t kFixedOverheadBytes = 512ULL * 1024ULL * 1024ULL;

// Reads one integer field out of genai_config.json.
//
// Deliberately not a JSON parser. Three integers are needed -- layer count,
// KV head count, head size -- and every general-purpose JSON library is a
// dependency this project does not otherwise have, for a file that GenAI
// itself writes and whose shape is fixed. The scan is strict about finding the
// key and about the value being digits, and reports failure rather than
// guessing, so a future format change surfaces as a fallback to the documented
// default instead of as a wrong memory figure that only shows up as an
// out-of-memory error much later.
bool readJsonInteger(const std::string& text, const std::string& key, std::int64_t& out) {
    const std::string quoted = "\"" + key + "\"";
    std::size_t at = text.find(quoted);
    if (at == std::string::npos) {
        return false;
    }
    at = text.find(':', at + quoted.size());
    if (at == std::string::npos) {
        return false;
    }
    ++at;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t'
                                || text[at] == '\n' || text[at] == '\r')) {
        ++at;
    }
    const std::size_t start = at;
    while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at])) != 0) {
        ++at;
    }
    if (at == start) {
        return false;
    }
    out = std::stoll(text.substr(start, at - start));
    return true;
}

// Reads one string field out of genai_config.json, for the same reason and with
// the same strictness as readJsonInteger: a missing or unterminated value
// leaves `out` untouched, so the caller keeps its default rather than loading
// half a name.
bool readJsonString(const std::string& text, const std::string& key, std::string& out) {
    const std::string quoted = "\"" + key + "\"";
    std::size_t at = text.find(quoted);
    if (at == std::string::npos) {
        return false;
    }
    at = text.find(':', at + quoted.size());
    if (at == std::string::npos) {
        return false;
    }
    ++at;
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t'
                                || text[at] == '\n' || text[at] == '\r')) {
        ++at;
    }
    if (at >= text.size() || text[at] != '"') {
        return false;
    }
    const std::size_t start = ++at;
    while (at < text.size() && text[at] != '"') {
        // A backslash escape is kept verbatim; these are file names and none
        // contain one, and unescaping properly would be more machinery than the
        // field is worth.
        if (text[at] == '\\' && at + 1 < text.size()) {
            ++at;
        }
        ++at;
    }
    if (at >= text.size() || at == start) {
        return false;
    }
    out = text.substr(start, at - start);
    return true;
}

std::string readWholeFile(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        return {};
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

// Quantisation suffixes a published model may use in place of the plain one,
// most preferred first. q4f16 is the usual choice on a GPU: four-bit weights
// with fp16 activations, which is the only combination that both fits a
// consumer card and uses the tensor cores.
constexpr std::string_view kQuantVariants[] = {
    "q4f16", "q4", "quantized", "int4", "fp16", "q8",
};

// Finds the file a decoder entry names, or the quantisation variant of it.
//
// genai_config.json names one file, and published packages do not always ship
// that file. The official onnx-community Qwen3-0.6B-DQ package, for instance,
// ships onnx/model_q4f16.onnx while its own genai_config.json names
// model.onnx -- and ONNX Runtime GenAI does not resolve the suffix, so the
// load fails with "File doesn't exist" for a model that is present and
// complete.
//
// Rather than making every user hand-edit a published package, the named file
// is checked and, when absent, the same stem with a quantisation suffix is
// looked for under the model root. The search is bounded to files that have a
// sibling external-data file, because a graph whose *.onnx_data is missing is
// not a loadable model and picking it would trade one clear error for a
// confusing one.
//
// Returns a path relative to `root`, in the form genai_config.json uses, or an
// empty string when nothing suitable is there.
std::string resolveDecoderFilename(const std::filesystem::path& root, std::string_view named) {
    if (std::filesystem::is_regular_file(root / std::filesystem::path(named))) {
        return std::string(named);
    }

    const std::filesystem::path wanted(named);
    const std::string stem = wanted.stem().string();
    const std::string extension = wanted.extension().string();
    if (stem.empty() || extension.empty()) {
        return {};
    }

    // Preferred variants first, then anything else sharing the stem, so a
    // package using a suffix nobody listed still loads.
    auto matches = [&](const std::filesystem::path& candidate) {
        const std::string name = candidate.filename().string();
        if (candidate.stem().string().rfind(stem + "_", 0) != 0) {
            return false;
        }
        if (candidate.extension().string() != extension) {
            return false;
        }
        // External data beside it, or it is not a complete model.
        std::filesystem::path data = candidate;
        data.replace_extension(candidate.extension().string() + "_data");
        return std::filesystem::is_regular_file(data);
    };

    std::error_code walkError;
    std::string fallback;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root, walkError)) {
        if (!entry.is_regular_file() || !matches(entry.path())) {
            continue;
        }
        const std::string name = entry.path().stem().string();
        for (const std::string_view variant : kQuantVariants) {
            if (name == stem + "_" + std::string(variant)) {
                return std::filesystem::relative(entry.path(), root).generic_string();
            }
        }
        if (fallback.empty()) {
            fallback = std::filesystem::relative(entry.path(), root).generic_string();
        }
    }
    return fallback;
}

// Bytes of KV cache one token occupies, across the whole model.
//
// One K row and one V row per layer per cached token, each
// kvHeads * headSize elements, held as fp16 -- which is what ONNX Runtime uses
// for the attention cache of an int4 model, whose weights are quantised but
// whose activations and cache are not. Under grouped-query attention the rows
// are far narrower than the model's hidden size, which is why a KV cache is
// much smaller than the weights it serves and why context length is affordable
// at all on a card this size.
std::size_t kvBytesPerToken(std::int64_t layers, std::int64_t kvHeads, std::int64_t headSize) {
    if (layers <= 0 || kvHeads <= 0 || headSize <= 0) {
        return 0;
    }
    constexpr std::size_t kBytesPerFp16 = 2;
    constexpr std::size_t kKeyAndValue = 2;
    const auto elements = static_cast<std::size_t>(kvHeads) * static_cast<std::size_t>(headSize);
    return static_cast<std::size_t>(layers) * kKeyAndValue * elements * kBytesPerFp16;
}

} // namespace

struct OrtGenAiBackend::Impl {
#ifdef KESTREL_HAS_ORT_GENAI
    std::unique_ptr<OgaModel> model;
    std::unique_ptr<OgaTokenizer> tokenizer;
#endif
    std::string provider;
    // What genai_config.json declares, which is the model's design maximum and
    // is frequently far larger than the machine.
    int declaredContextLength = 0;
    // The device facts the budget was computed from, kept for the status line
    // so the number can be explained rather than merely reported.
    std::size_t weightsBytes = 0;
    std::size_t totalDeviceBytes = 0;
    std::size_t kvBytesPerTokenValue = 0;
    // The decoder file the config named, and the one actually loaded, when they
    // differ. Reported so a quantisation substitution is visible rather than
    // silent.
    std::string resolvedDecoder;
};

OrtGenAiBackend::OrtGenAiBackend() {
    m_impl = std::make_unique<Impl>();
    m_status.backendName = "ONNX Runtime GenAI";
#ifdef KESTREL_HAS_ORT_GENAI
    m_status.available = true;
    m_status.detail = "ONNX Runtime GenAI linked; no model loaded";
#else
    m_status.detail = "ONNX Runtime GenAI not compiled in "
                      "(configure with -DKESTREL_ORT_GENAI_ROOT=<install tree>)";
#endif
    refreshStatus();
}

OrtGenAiBackend::~OrtGenAiBackend() = default;

BackendKind OrtGenAiBackend::kind() const noexcept {
    return BackendKind::OrtGenAI;
}

RuntimeStatus OrtGenAiBackend::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void OrtGenAiBackend::setContextLengthForTesting(int tokens) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_requestedContextLength = tokens > 0 ? tokens : 0;
    refreshStatus();
}

void OrtGenAiBackend::refreshStatus() {
    // Caller holds m_mutex.
    m_status.contextLimit = static_cast<std::size_t>(std::max(m_contextLength, 0));
    m_status.contextUsed = m_contextUsed;
    m_status.kvCacheBytes = m_kvCacheBytes;
    m_status.kvCacheBytesUsed = m_kvCacheBytesUsed;
}

#ifdef KESTREL_HAS_ORT_GENAI

bool OrtGenAiBackend::loadModel(const std::string& modelPath, std::string& error) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cancelled.store(false);
    m_status.modelLoaded = false;
    m_contextUsed = 0;
    m_kvCacheBytesUsed = 0;
    m_contextLength = 0;
    m_kvCacheBytes = 0;

    std::error_code fileError;
    const std::filesystem::path root(modelPath);
    if (!std::filesystem::is_directory(root, fileError)) {
        error = "Not a model directory: " + modelPath
              + " (an ONNX Runtime GenAI model is a folder holding "
                "genai_config.json and model.onnx, not a single file)";
        m_status.detail = error;
        return false;
    }
    const std::filesystem::path configPath = root / "genai_config.json";
    if (!std::filesystem::is_regular_file(configPath, fileError)) {
        error = "No genai_config.json in " + modelPath
              + ". This looks like a plain ONNX export; ONNX Runtime GenAI needs "
                "the model builder's layout, which includes genai_config.json.";
        m_status.detail = error;
        return false;
    }

    const std::string configText = readWholeFile(configPath);
    if (configText.empty()) {
        error = "genai_config.json in " + modelPath + " is empty or unreadable";
        m_status.detail = error;
        return false;
    }

    // Execution provider. CUDA when this build has the toolkit and the machine
    // has a device, because a 4B int4 model on the CPU is a slideshow; the CPU
    // provider is a real answer rather than a consolation prize, and is what
    // makes this backend work on a machine with no GPU at all.
    const CudaProbe probe = probeCuda();
    const bool useCuda =
#ifdef KESTREL_HAS_CUDA
        probe.hasDevice();
#else
        false;
#endif

    m_impl->provider = useCuda ? "cuda" : "cpu";
    if (useCuda) {
        const CudaDeviceInfo* device = probe.selectedDevice();
        if (device != nullptr) {
            m_impl->totalDeviceBytes = device->totalMemoryBytes;
            // Select the device explicitly. Without this the provider picks
            // device 0, which is not necessarily the one probeCuda() chose, and
            // the two disagreeing is invisible until the run is slow.
            try {
                OgaCheckResult(OgaSetCurrentGpuDeviceId(device->index));
            } catch (const std::exception&) {
                // A failure here is not fatal: the provider falls back to its
                // own default. Recorded rather than swallowed, because the
                // resulting run will be on a GPU the user did not expect.
                m_impl->provider = "cuda (device selection refused)";
            }
        }
    }

    // Weights on disk. Summed over the directory rather than taken from one
    // file, because a GenAI model is a graph plus external data and the data
    // file is where essentially all of it lives.
    std::size_t weightBytes = 0;
    for (const auto& entry : std::filesystem::directory_iterator(root, fileError)) {
        if (entry.is_regular_file() && entry.path().extension() != ".json"
            && entry.path().extension() != ".jinja") {
            weightBytes += static_cast<std::size_t>(entry.file_size());
        }
    }
    m_impl->weightsBytes = weightBytes;

    // Model geometry, for the KV arithmetic. Every field is optional; a missing
    // one falls the budget back to the documented default rather than
    // producing a fabricated figure.
    std::int64_t layers = 0;
    std::int64_t kvHeads = 0;
    std::int64_t headSize = 0;
    std::int64_t declaredContext = 0;
    readJsonInteger(configText, "context_length", declaredContext);
    readJsonInteger(configText, "num_hidden_layers", layers);
    readJsonInteger(configText, "num_key_value_heads", kvHeads);
    readJsonInteger(configText, "head_size", headSize);
    m_impl->declaredContextLength = declaredContext > 0 ? static_cast<int>(declaredContext) : 0;
    m_impl->kvBytesPerTokenValue = kvBytesPerToken(layers, kvHeads, headSize);

    // Read once, outside the try, because the status line below reports any
    // substitution made to it and the value has to survive the block.
    std::string decoderName = "model.onnx";
    readJsonString(configText, "filename", decoderName);

    try {
        m_impl->model.reset();
        m_impl->tokenizer.reset();

        // Two layouts exist and both are published. A *flat* directory holds
        // genai_config.json, model.onnx and tokenizer.json directly; a
        // *package* holds them inside a nested folder. They need different
        // constructors and neither constructor accepts the other's layout, and
        // the failure message names the distinction without saying which
        // constructor to use -- so both are tried and the flat one goes first,
        // being the more common.
        //
        // Assuming one is how this backend reported a model as unloadable on
        // the first attempt: the flat form against a package-shaped model, or
        // the other way round, and a perfectly good 3.7 GB model refused.
        std::unique_ptr<OgaConfig> config;
        std::string firstFailure;
        try {
            config = OgaConfig::Create(modelPath.c_str());
        } catch (const std::exception& thrown) {
            firstFailure = thrown.what();
            try {
                config = OgaConfig::CreateFromPackageEp(modelPath.c_str(),
                                                       m_impl->provider.c_str());
            } catch (const std::exception&) {
                // Report the flat-directory failure: it is the one that describes
                // what was actually found on disk.
                throw std::runtime_error(firstFailure);
            }
        }

        // The execution provider is set explicitly rather than left to whatever
        // genai_config.json happens to name. A model that configures the CUDA
        // provider on a machine with no GPU, or a CPU-only model on a machine
        // with one, is a configuration the user did not choose, and the status
        // line below then reports whichever provider actually ran.
        config->ClearProviders();
        config->AppendProvider(m_impl->provider.c_str());

        // Redirect the decoder to the file that is actually on disk. The
        // overlay is the supported way to change a loaded config, so the
        // package's own genai_config.json is left exactly as published and the
        // correction lives only in this process.
        const std::string resolved = resolveDecoderFilename(root, decoderName);        if (!resolved.empty() && resolved != decoderName) {
            config->Overlay(("{\"model\":{\"decoder\":{\"filename\":\""
                             + resolved + "\"}}}").c_str());
            m_impl->resolvedDecoder = resolved;
        } else {
            m_impl->resolvedDecoder = decoderName;
        }

        m_impl->model = OgaModel::Create(*config);
        m_impl->tokenizer = OgaTokenizer::Create(*config);
    } catch (const std::exception& thrown) {
        m_impl->model.reset();
        m_impl->tokenizer.reset();
        m_status.modelLoaded = false;
        error = "ONNX Runtime GenAI could not load " + modelPath + ": " + thrown.what();
        m_status.detail = error;
        return false;
    }

    // The context this machine can actually hold, which is usually not what
    // the model declares. Qwen3.5-4B declares 262144; on an 8 GB card with
    // 3.7 GB of weights resident that is not a setting, it is a way to fail at
    // load time. The declared figure is kept and reported, because "your model
    // supports 262144" and "your machine can hold 24576" are both true and
    // only the second one is actionable.
    int chosen = m_impl->declaredContextLength > 0 ? m_impl->declaredContextLength
                                                   : kFallbackContextLength;
    int budgeted = 0;
    if (m_impl->totalDeviceBytes > 0 && m_impl->kvBytesPerTokenValue > 0) {
        const std::size_t total = m_impl->totalDeviceBytes;
        const std::size_t spare = total > m_impl->weightsBytes + kFixedOverheadBytes
                                      ? total - m_impl->weightsBytes - kFixedOverheadBytes
                                      : 0;
        const auto affordable = static_cast<std::size_t>(
            static_cast<double>(spare) * kKvBudgetShare) / m_impl->kvBytesPerTokenValue;
        if (affordable >= 1024) {
            // Rounded down to a multiple of 1024 so the reported figure is one
            // a reader can recognise as a deliberate choice rather than the
            // residue of a division.
            budgeted = static_cast<int>((affordable / 1024) * 1024);
            chosen = std::min(chosen, budgeted);
        } else {
            // Not enough room for even a modest context. The load has already
            // succeeded, so the model is resident; generation is what will
            // strain, and the status says so rather than the load failing for
            // a reason the user cannot act on.
            budgeted = 0;
        }
    }
    if (m_requestedContextLength > 0) {
        chosen = m_requestedContextLength;
    }
    if (chosen < 1024) {
        chosen = 1024;
    }
    m_contextLength = chosen;
    m_kvCacheBytes = m_impl->kvBytesPerTokenValue * static_cast<std::size_t>(m_contextLength);

    m_modelPath = modelPath;
    m_status.modelLoaded = true;
    m_status.modelName = core::pathText(root.filename().string());
    refreshStatus();

    std::ostringstream detail;
    detail << "Loaded " << m_status.modelName << " on the " << m_impl->provider
           << " provider; context " << m_contextLength << " tokens ("
           << formatBytes(m_kvCacheBytes) << " of KV cache";
    if (m_impl->declaredContextLength > 0) {
        detail << "; the model declares " << m_impl->declaredContextLength;
    }
    if (m_requestedContextLength > 0) {
        detail << "; context was set explicitly to " << m_requestedContextLength;
    }
    if (budgeted > 0 && m_requestedContextLength == 0) {
        detail << ", this machine affords " << budgeted;
    }
    detail << ")";
    if (!m_impl->resolvedDecoder.empty()
        && m_impl->resolvedDecoder != decoderName) {
        detail << "; loaded " << m_impl->resolvedDecoder << " in place of the "
               << decoderName << " the config names";
    }
    m_status.detail = detail.str();    error.clear();
    return true;
}

void OrtGenAiBackend::generate(const GenerationRequest& request,
                               TokenCallback onToken,
                               CompletionCallback onComplete) {
    std::unique_lock<std::mutex> lock(m_mutex);
    m_cancelled.store(false);

    if (m_impl == nullptr || m_impl->model == nullptr || m_impl->tokenizer == nullptr) {
        onComplete(false, "No ONNX Runtime GenAI model is loaded. Call loadModel() first.");
        return;
    }
    if (request.prompt.empty()) {
        onComplete(false, "The request carried no prompt.");
        return;
    }

    // The system prompt is prepended here rather than by the caller, so the
    // ModelBackend contract holds: callers pass the per-turn prompt only.
    std::string text = m_systemPrompt;
    if (!text.empty()) {
        text += "\n\n";
    }
    text += request.prompt;

    try {
        auto sequences = OgaSequences::Create();
        m_impl->tokenizer->Encode(text.c_str(), *sequences);
        const std::size_t inputCount = sequences->SequenceCount(0);
        if (inputCount == 0) {
            onComplete(false, "The prompt tokenized to nothing.");
            return;
        }

        auto params = OgaGeneratorParams::Create(*m_impl->model);
        // max_length counts prompt plus reply in this API, so a reply budget has
        // to be added to the prompt rather than used as the total.
        const auto maxLength = static_cast<double>(inputCount)
                             + static_cast<double>(std::max(request.maxTokens, 1));
        params->SetSearchOption("max_length", maxLength);
        params->SetSearchOption("temperature", static_cast<double>(request.temperature));
        // Sampling only when a temperature was actually chosen above zero.
        // Greedy at zero is both faster and the more predictable of the two,
        // and a warmup asking for eight tokens has no use for variety.
        params->SetSearchOptionBool("do_sample", request.temperature > 0.0F);

        auto generator = OgaGenerator::Create(*m_impl->model, *params);
        generator->AppendTokenSequences(*sequences);

        m_contextUsed = inputCount;
        m_kvCacheBytesUsed = m_impl->kvBytesPerTokenValue * inputCount;
        refreshStatus();

        // Streaming decode. OgaTokenizerStream is the correct tool rather than
        // decoding the whole sequence each step: it holds the partial UTF-8
        // state between calls, so a character split across two tokens is
        // emitted once, whole, instead of appearing as a replacement character
        // and then being corrected a token later.
        auto stream = OgaTokenizerStream::Create(*m_impl->tokenizer);
        std::size_t emitted = inputCount;

        while (!generator->IsDone()) {
            if (m_cancelled.load()) {
                m_contextUsed = 0;
                m_kvCacheBytesUsed = 0;
                refreshStatus();
                onComplete(false, "Generation cancelled");
                return;
            }
            generator->GenerateNextToken();
            const std::size_t produced = generator->GetSequenceCount(0);
            if (produced > emitted) {
                for (std::size_t i = emitted; i < produced; ++i) {
                    const char* chunk = stream->Decode(generator->GetSequenceData(0)[i]);
                    if (chunk != nullptr && *chunk != '\0') {
                        onToken(std::string_view(chunk));
                    }
                }
                emitted = produced;
            }
        }

        const std::size_t finalLength = generator->GetSequenceCount(0);
        m_contextUsed = finalLength;
        m_kvCacheBytesUsed = m_impl->kvBytesPerTokenValue * finalLength;
        refreshStatus();
        onComplete(true, {});
    } catch (const std::exception& thrown) {
        m_contextUsed = 0;
        m_kvCacheBytesUsed = 0;
        refreshStatus();
        onComplete(false, thrown.what());
    }
}

void OrtGenAiBackend::cancel() {
    m_cancelled.store(true);
}

std::size_t OrtGenAiBackend::countTokens(std::string_view text) const {
    std::lock_guard<std::mutex> lock(m_mutex);
#ifdef KESTREL_HAS_ORT_GENAI
    if (m_impl == nullptr || m_impl->tokenizer == nullptr || text.empty()) {
        return 0;
    }
    try {
        auto sequences = OgaSequences::Create();
        m_impl->tokenizer->Encode(std::string(text).c_str(), *sequences);
        return sequences->SequenceCount(0);
    } catch (const std::exception&) {
        // A tokenizer that cannot answer must not take the caller down with
        // it; the base class's approximation is the documented fallback.
        return 0;
    }
#else
    static_cast<void>(text);
    return 0;
#endif
}

void OrtGenAiBackend::resetContextUsage() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_contextUsed = 0;
    m_kvCacheBytesUsed = 0;
    refreshStatus();
}

void OrtGenAiBackend::setSystemPrompt(std::string_view text) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_systemPrompt = std::string(text);
}

std::size_t OrtGenAiBackend::cachedPrefixTokens() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    // Zero, and the distinction matters. The system prompt is prepended to
    // every turn but is not kept resident between them, so reporting its token
    // count here would claim a KV-cache saving that does not happen.
    //
    // Keeping it resident is possible -- OgaGenerator::RewindTo() exists for
    // exactly this -- but it constrains the generator to outlive a turn, and
    // with it the search options that are set once at creation. Temperature
    // arrives per request, so a generator that survived the turn would freeze
    // the first turn's temperature for the rest of the conversation. Paying the
    // re-decode is cheaper than a reply that ignores what the user asked for.
    return 0;
}

#else // !KESTREL_HAS_ORT_GENAI

// Every method still exists in a build without ONNX Runtime GenAI, and every one
// of them reports the same thing. A backend that is compiled but inert is
// worse than one that is absent: the registry would rank it, selectBackend()
// would hand it a model, and the failure would arrive at the first reply
// instead of at startup where it can be read on a diagnostics line.
bool OrtGenAiBackend::loadModel(const std::string& modelPath, std::string& error) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cancelled.store(false);
    m_status.modelLoaded = false;
    error = "ONNX Runtime GenAI is not compiled into this build, so " + modelPath
          + " cannot be loaded. Configure with -DKESTREL_ORT_GENAI_ROOT=<install tree>.";
    m_status.detail = error;
    return false;
}

void OrtGenAiBackend::generate(const GenerationRequest& request,
                               TokenCallback onToken,
                               CompletionCallback onComplete) {
    static_cast<void>(request);
    static_cast<void>(onToken);
    std::lock_guard<std::mutex> lock(m_mutex);
    m_cancelled.store(false);
    onComplete(false, "ONNX Runtime GenAI is not compiled into this build.");
}

void OrtGenAiBackend::cancel() {
    m_cancelled.store(true);
}

void OrtGenAiBackend::setContextLengthForTesting(int tokens) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_requestedContextLength = tokens > 0 ? tokens : 0;
}

#endif // KESTREL_HAS_ORT_GENAI

} // namespace kestrel::runtime
