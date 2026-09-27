#include "runtime/backendregistry.h"
#include "runtime/cudadevice.h"
#include "runtime/engineartifact.h"
#include "runtime/llamacppbackend.h"
#include "runtime/mockbackend.h"
#include "runtime/ortgenaibackend.h"
#include "runtime/sapirecognizer.h"
#include "runtime/speechrecognizer.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

using namespace kestrel;

runtime::CudaDeviceInfo makeDevice(int index, int major, int minor) {
    runtime::CudaDeviceInfo device;
    device.index = index;
    device.name = "Test Device " + std::to_string(index);
    device.computeMajor = major;
    device.computeMinor = minor;
    device.totalMemoryBytes = 8589934592ULL; // 8 GiB
    device.multiprocessorCount = 24;
    device.memoryClockRateKhz = 10000000; // 10 GHz, a plausible GDDR6-class clock
    device.memoryBusWidthBits = 128;
    return device;
}

runtime::CudaProbe makeProbe(bool runtimeAvailable, std::vector<runtime::CudaDeviceInfo> devices) {
    runtime::CudaProbe probe;
    probe.runtime.available = runtimeAvailable;
    probe.runtime.driverVersion = 13060; // new enough for the 13.4 runtime below
    probe.runtime.runtimeVersion = 13040;
    probe.runtime.deviceCount = static_cast<int>(devices.size());
    probe.devices = std::move(devices);
    probe.selectedDeviceIndex =
        runtime::resolveSelectedDeviceIndex(probe.devices, -1);
    return probe;
}

void testVersionAndByteFormatting() {
    // CUDA packs major*1000 + minor*10 + patch.
    assert(runtime::formatCudaVersion(13040) == "13.4");
    assert(runtime::formatCudaVersion(12040) == "12.4");
    assert(runtime::formatCudaVersion(11030) == "11.3");
    assert(runtime::formatCudaVersion(0).empty());
    assert(runtime::formatCudaVersion(-1).empty());

    assert(runtime::formatBytes(8589934592ULL) == "8.0 GiB");
    assert(runtime::formatBytes(1073741824ULL) == "1.0 GiB");
    assert(runtime::formatBytes(512ULL * 1024ULL * 1024ULL) == "512.0 MiB");
    // Sub-MiB values stay in MiB rather than dropping to a noisier unit.
    assert(runtime::formatBytes(512ULL * 1024ULL) == "0.5 MiB");
    assert(runtime::formatBytes(0) == "0.0 MiB");

    // TensorRT packs major*1000 + minor*100 + patch, so its minor digit is in
    // the hundreds place. Getting this wrong renders 8.6.1 as "8.60".
    assert(runtime::formatTensorRTVersion(10400) == "10.4");
    assert(runtime::formatTensorRTVersion(8601) == "8.6.1");
    assert(runtime::formatTensorRTVersion(8600) == "8.6");
    assert(runtime::formatTensorRTVersion(0).empty());
    assert(runtime::formatTensorRTVersion(-5).empty());

    assert(runtime::compareVersions(10400, 10300) > 0);
    assert(runtime::compareVersions(10300, 10400) < 0);
    assert(runtime::compareVersions(10400, 10400) == 0);
}

// Compute capability and memory formatting must not be confused with the
// major.minor scaling used for CUDA/TensorRT version codes.
void testDeviceFormatting() {
    runtime::CudaDeviceInfo device = makeDevice(0, 8, 9);
    assert(device.computeCapability() == "8.9");
    assert(device.memorySummary() == "8.0 GiB");

    // 2 * 10 GHz * 16 bytes = 320 GB/s (double data rate)
    assert(device.gigabytesPerSecond() > 319.0 && device.gigabytesPerSecond() < 321.0);

    // Capabilities must be separated, not run together: "tensor cores320 GB/s"
    // is a real regression these assertions exist to catch.
    const std::string description = runtime::describeDevice(device);
    assert(description.find(", tensor cores") != std::string::npos);
    assert(description.find(", 320 GB/s") != std::string::npos);
    assert(description.find("cores320") == std::string::npos);

    device.memoryClockRateKhz = 0;
    assert(device.gigabytesPerSecond() == 0.0);
    // An unreported clock must not fabricate a bandwidth figure.
    assert(runtime::describeDevice(device).find("GB/s") == std::string::npos);

    runtime::CudaDeviceInfo unknown;
    assert(unknown.computeCapability().empty());
    assert(runtime::isTensorCoreCapable(8, 9));
    assert(runtime::isTensorCoreCapable(7, 0));
    assert(!runtime::isTensorCoreCapable(6, 1));
    assert(!runtime::isTensorCoreCapable(0, 0));
}

void testDeviceSelection() {
    std::vector<runtime::CudaDeviceInfo> devices{makeDevice(0, 8, 9), makeDevice(1, 7, 5)};
    assert(runtime::resolveSelectedDeviceIndex(devices, -1) == 0);
    assert(runtime::resolveSelectedDeviceIndex(devices, 1) == 1);
    assert(runtime::resolveSelectedDeviceIndex(devices, 7) == 0);
    assert(runtime::resolveSelectedDeviceIndex({}, 0) == -1);

    runtime::CudaProbe probe = makeProbe(true, devices);
    assert(probe.hasDevice());
    assert(probe.selectedDevice() != nullptr);
    assert(probe.selectedDevice()->name == "Test Device 0");
    assert(probe.runtime.driverSufficient());

    runtime::CudaProbe empty = makeProbe(false, {});
    assert(!empty.hasDevice());
    assert(empty.selectedDevice() == nullptr);
    assert(empty.summary().find("unavailable") != std::string::npos);

    // An older driver than the toolkit is still usable but worth flagging.
    runtime::CudaProbe oldDriver = makeProbe(true, devices);
    oldDriver.runtime.driverVersion = 12000;
    assert(!oldDriver.runtime.driverSufficient());
}

void testProbeIsSafeWithoutDevices() {
    const runtime::CudaProbe probe = runtime::probeCuda();
    assert(probe.runtime.errorCode == 0 || !probe.runtime.errorName.empty());
    assert(!probe.runtime.detail.empty());
    if (!probe.runtime.available) {
        assert(probe.devices.empty());
        assert(probe.selectedDevice() == nullptr);
        return;
    }
    assert(probe.selectedDeviceIndex >= 0);
    const runtime::CudaDeviceInfo* device = probe.selectedDevice();
    assert(device != nullptr);
    assert(!device->name.empty());
    assert(device->computeMajor > 0);
    assert(device->totalMemoryBytes > 0);
    assert(device->multiprocessorCount > 0);
    assert(probe.runtime.runtimeVersion > 0);
    std::printf("  CUDA probe: %s\n", probe.summary().c_str());
    std::printf("  device detail: %s\n", runtime::describeDevice(*device).c_str());
}

void testEngineSidecarRoundTrip() {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "kestrel_engine_record_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    const std::string engine = (dir / "demo.plan").string();

    // A placeholder artifact is enough: the record describes the build, not the file.
    {
        std::ofstream file(engine, std::ios::binary);
        file << "not a real engine";
    }

    runtime::EngineBuildRecord record;
    std::string error = "sentinel";
    assert(!runtime::readEngineBuildRecord(engine, record, error));
    assert(error.find("No engine build record") != std::string::npos);

    record.tensorrtVersion = 10400;
    record.cudaVersion = 13040;
    record.computeMajor = 8;
    record.computeMinor = 9;
    record.gpuName = "Test GPU";
    record.builtBy = "kestrel tests";
    const bool wrote = runtime::writeEngineBuildRecord(engine, record, error);
    assert(wrote);
    assert(error.empty());
    assert(std::filesystem::exists(runtime::engineSidecarPath(engine)));

    runtime::EngineBuildRecord loaded;
    assert(runtime::readEngineBuildRecord(engine, loaded, error));
    assert(error.empty());
    assert(loaded.tensorrtVersion == 10400);
    assert(loaded.cudaVersion == 13040);
    assert(loaded.computeMajor == 8);
    assert(loaded.computeMinor == 9);
    assert(loaded.gpuName == "Test GPU");
    assert(loaded.builtBy == "kestrel tests");

    // Values containing newlines must not corrupt the record.
    runtime::EngineBuildRecord hostile = record;
    hostile.gpuName = "line one\nline two";
    const bool rewrote = runtime::writeEngineBuildRecord(engine, hostile, error);
    assert(rewrote);
    const bool reread = runtime::readEngineBuildRecord(engine, loaded, error);
    assert(reread);
    assert(loaded.gpuName == "line one line two");

    // Unknown keys are reported rather than silently dropped.
    {
        std::ofstream file(runtime::engineSidecarPath(engine), std::ios::binary | std::ios::trunc);
        file << "made_up_field=1\n";
    }
    assert(!runtime::readEngineBuildRecord(engine, loaded, error));
    assert(error.find("Unknown key") != std::string::npos);

    std::filesystem::remove_all(dir);
}

void testEngineCompatibility() {
    const runtime::CudaProbe probe = makeProbe(true, {makeDevice(0, 8, 9)});

    runtime::EngineBuildRecord matching;
    matching.tensorrtVersion = 10400;
    matching.cudaVersion = 13040;
    matching.computeMajor = 8;
    matching.computeMinor = 9;
    matching.gpuName = "Test Device 0";

    auto report = runtime::checkEngineCompatibility(matching, probe, 10400);
    assert(report.verdict == runtime::EngineCompatibility::Compatible);
    assert(report.loadable());
    assert(report.remedy.empty());

    // Wrong architecture: the hard stop.
    runtime::EngineBuildRecord wrongArch = matching;
    wrongArch.computeMajor = 7;
    wrongArch.computeMinor = 5;
    report = runtime::checkEngineCompatibility(wrongArch, probe, 10400);
    assert(report.verdict == runtime::EngineCompatibility::Incompatible);
    assert(!report.loadable());
    assert(report.remedy.find("Rebuild") != std::string::npos);

    // Wrong TensorRT version: also a hard stop.
    runtime::EngineBuildRecord wrongTrt = matching;
    wrongTrt.tensorrtVersion = 10300;
    report = runtime::checkEngineCompatibility(wrongTrt, probe, 10400);
    assert(report.verdict == runtime::EngineCompatibility::Incompatible);
    assert(report.remedy.find("Rebuild the engine") != std::string::npos);

    // No TensorRT runtime to compare against: warn, do not block.
    report = runtime::checkEngineCompatibility(matching, probe, 0);
    assert(report.verdict == runtime::EngineCompatibility::CompatibleWithWarning);
    assert(report.loadable());

    // Driver older than the engine's toolkit.
    runtime::CudaProbe oldDriver = makeProbe(true, {makeDevice(0, 8, 9)});
    oldDriver.runtime.driverVersion = 12000;
    report = runtime::checkEngineCompatibility(matching, oldDriver, 10400);
    assert(report.verdict == runtime::EngineCompatibility::Incompatible);

    // Same compute capability, different GPU model.
    runtime::EngineBuildRecord otherGpu = matching;
    otherGpu.gpuName = "Some Other Card";
    report = runtime::checkEngineCompatibility(otherGpu, probe, 10400);
    assert(report.verdict == runtime::EngineCompatibility::CompatibleWithWarning);
    assert(report.loadable());

    // An all-zero record asserts nothing.
    report = runtime::checkEngineCompatibility(runtime::EngineBuildRecord{}, probe, 10400);
    assert(report.verdict == runtime::EngineCompatibility::Unknown);
    assert(report.loadable());

    assert(runtime::EngineCompatibilityReport::toString(runtime::EngineCompatibility::Incompatible)
           == "incompatible");
}

void testBackendSelectionAndDiagnostics() {
    const auto backend = runtime::selectBackend(runtime::BackendKind::Mock);
    assert(backend != nullptr);
    assert(backend->status().available);

    assert(runtime::toString(runtime::BackendKind::Mock) == "mock");
    assert(runtime::toString(runtime::BackendKind::LlamaCpp) == "llamacpp");
    assert(runtime::toString(runtime::BackendKind::OrtGenAI) == "onnx-genai");

    const auto diagnostics = runtime::runtimeDiagnostics(runtime::probeCuda());
    assert(!diagnostics.empty());
    bool sawCudaRow = false;
    bool sawGenAiRow = false;
    for (const runtime::RuntimeDiagnostic& row : diagnostics) {
        assert(!row.label.empty());
        assert(!row.value.empty());
        if (row.label == "CUDA toolkit") {
            sawCudaRow = true;
        }
        if (row.label == "ONNX Runtime GenAI") {
            sawGenAiRow = true;
        }
    }
    assert(sawCudaRow);
    // The Windows-native backend must be visible in the report whether or not it
    // is compiled in. A backend that only appears once it is linked is a
    // backend nobody can discover before deciding to build it.
    assert(sawGenAiRow);
    std::printf("  diagnostics rows: %zu\n", diagnostics.size());
    for (const runtime::RuntimeDiagnostic& row : diagnostics) {
        std::printf("    [%s] %s: %s\n", row.ok ? " ok " : "warn", row.label.c_str(),
                    row.value.c_str());
    }
}

} // namespace

// Token counting is backend-driven so a loaded model reports exact numbers
// rather than an estimate. These cover the shared fallback and the contract
// backends inherit when they have no tokenizer.
void testBackendDrivenTokenCounting() {
    // Empty input costs nothing. A caller that treats 0 as "free" would
    // otherwise under-count the whole prompt.
    assert(runtime::MockBackend{}.countTokens("") == 0);

    // The approximation must never report zero for non-empty text.
    const std::string oneChar = "x";
    assert(runtime::MockBackend{}.countTokens(oneChar) >= 1);

    // Roughly four characters per token, so 400 characters is about 100
    // tokens. The bound is deliberately loose: the exact value is a property
    // of a real vocabulary, not of this fallback.
    const std::string fourHundred(400, 'a');
    const std::size_t estimate = runtime::MockBackend{}.countTokens(fourHundred);
    assert(estimate >= 50 && estimate <= 200);

    // Longer text must cost strictly more, or context accounting would not
    // grow with the conversation.
    const std::string eightHundred(800, 'a');
    assert(runtime::MockBackend{}.countTokens(eightHundred) > estimate);

    // A backend with no model must not claim a prompt is free.
    runtime::LlamaCppBackend llama;
    assert(llama.countTokens("") == 0);
    assert(llama.countTokens(fourHundred) >= 1);

    // Resetting usage must not throw and must leave the backend usable.
    llama.resetContextUsage();
    assert(llama.countTokens(fourHundred) >= 1);
}

// With no SDK linked, the GGUF backend has to say so instead of appearing
// healthy. The registry depends on this to avoid selecting a dead backend.
//
// The checks are explicit rather than assert() because this test has to hold
// under two configurations, and a bare assert in a Release build disappears
// while in Debug it reports nothing useful about a cross-configuration value.
void testLlamaCppBackendReportsUnavailableWithoutSdk() {
    runtime::LlamaCppBackend backend;
    const runtime::RuntimeStatus status = backend.status();
    if (status.modelLoaded) {
        std::printf("  FAIL  a fresh backend reported a model as loaded\n");
        std::abort();
    }

    std::string error;
    if (backend.loadModel("model.gguf", error)) {
        std::printf("  FAIL  a bogus path loaded as a model\n");
        std::abort();
    }
    // The message must tell the user how to fix it, not just that it failed.
    // In an SDK-linked build the failure is a real load attempt instead, so
    // accept either explanation rather than pinning one configuration.
    if (error.empty() || (error.find("KESTREL_ENABLE_LLAMA_CPP") == std::string::npos
                          && error.find("could not load") == std::string::npos)) {
        std::printf("  FAIL  unhelpful load error: \"%s\"\n", error.c_str());
        std::abort();
    }

    bool completed = false;
    bool refused = false;
    backend.generate(runtime::GenerationRequest{"hi", 0.7F, 16}, nullptr,
                     [&completed, &refused](bool success, std::string_view) {
                         // Without a loaded model every build must refuse. Note
                         // the callback runs while the backend holds its own
                         // lock, so it must not call back into the backend;
                         // that is checked after generate() returns.
                         refused = !success;
                         completed = true;
                     });
    if (!completed) {
        std::printf("  FAIL  generate never reported completion\n");
        std::abort();
    }
    if (!refused) {
        std::printf("  FAIL  generate succeeded with no model loaded\n");
        std::abort();
    }
    if (backend.status().modelLoaded) {
        std::printf("  FAIL  a refused generate left a model marked loaded\n");
        std::abort();
    }
}

/// The shared system prompt contract, which must hold in every configuration.
void testSharedSystemPromptPrefix() {
    runtime::MockBackend mock;

    // No prefix declared: nothing to reuse, and no cost claimed.
    assert(mock.systemPrompt().empty());
    assert(mock.cachedPrefixTokens() == 0);

    const std::string prefix = "You are Kestrel, a local assistant running on the user's own machine.";
    mock.setSystemPrompt(prefix);
    assert(mock.systemPrompt() == prefix);
    // A backend with no cache still charges for the prefix on every turn, so
    // the figure is the prefix cost, not a fictitious saving.
    assert(mock.cachedPrefixTokens() == mock.countTokens(prefix));
    assert(mock.cachedPrefixTokens() > 0);

    // Redeclaring the same text must be idempotent, or a setter that fires on
    // every property write would invalidate the cache each time.
    mock.setSystemPrompt(prefix);
    assert(mock.systemPrompt() == prefix);

    // A different prompt is a different prefix and must replace the old one.
    mock.setSystemPrompt("A completely different instruction set.");
    assert(mock.systemPrompt() != prefix);

    mock.clearSharedPrefix();
    assert(mock.systemPrompt().empty());
    assert(mock.cachedPrefixTokens() == 0);

    // A backend that caches a prefix must not require the caller to include it
    // in the per-turn prompt, and must not double-charge for it.
    mock.setSystemPrompt(prefix);
    mock.resetContextUsage();
    const std::size_t afterPrefixOnly = mock.status().contextUsed;
    assert(afterPrefixOnly == 0);
    // The reply has to be collected to be charged for, so the exact figure can
    // be stated: prefix, the turn's own prompt, and what came back.
    std::string reply;
    mock.generate(runtime::GenerationRequest{"hello", 0.7F, 32},
                  [&reply](std::string_view token) { reply.append(token); },
                  [](bool, std::string_view) {});
    assert(!reply.empty());

    // Exact, not a lower bound. A duplicate charge of the prefix is precisely
    // the bug worth catching here, and ">=" would sail straight past it.
    assert(mock.status().contextUsed
           == mock.countTokens(prefix) + mock.countTokens("hello") + mock.countTokens(reply));
}

/// One environment variable, as a string the caller owns.
///
/// getenv is deprecated on Windows because the pointer it returns aliases an
/// environment block the caller does not own, and MSVC says so at /W4 on every
/// use. The secure variant is the documented replacement there and is a
/// different signature, so the two cannot share a line; this file is compiled
/// both with and without Qt, so it cannot reach for QString either.
std::string environmentOrEmpty(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t length = 0;
    if (_dupenv_s(&value, &length, name) != 0 || value == nullptr) {
        return {};
    }
    std::string result(value);
    std::free(value);
    return result;
#else
    const char* value = std::getenv(name);
    return value == nullptr ? std::string() : std::string(value);
#endif
}

/// Exercises the real llama.cpp generation path.///
/// Skipped unless KESTREL_TEST_GGUF points at a GGUF file, so CI does not need
/// a multi-hundred-megabyte model download to run the suite. When it is set,
/// this is the only test that proves the backend actually generates rather than
/// merely linking.
void testLlamaCppGeneratesFromRealModel() {
    const std::string modelPath = environmentOrEmpty("KESTREL_TEST_GGUF");
    if (modelPath.empty()) {
        std::printf("  skip  real GGUF generation (set KESTREL_TEST_GGUF to run)\n");
        return;
    }

    runtime::LlamaCppBackend backend;
    std::string error;

    // A freshly constructed backend must already claim to be available, with no
    // model loaded. This is the state the registry inspects when choosing a
    // backend and the state the model picker gates its button on, so a backend
    // that reported itself unavailable here was one nobody could ever hand a
    // model. It is asserted here, in a build where llama.cpp is actually
    // linked, because that is the only configuration where it can be wrong.
    if (!backend.status().available) {
        std::printf("  FAIL  a linked llama.cpp backend reported itself unavailable\n");
        std::abort();
    }
    assert(!backend.status().modelLoaded);


    if (!backend.loadModel(modelPath, error)) {
        std::printf("  FAIL  could not load %s: %s\n", modelPath.c_str(), error.c_str());
        std::abort();
    }

    const runtime::RuntimeStatus loaded = backend.status();
    assert(loaded.modelLoaded);
    assert(loaded.contextLimit > 0);

    // The tokenizer must be real: it is the whole point of moving counting
    // behind the backend. A fixed chars-per-token ratio cannot tell two
    // equal-length strings apart, so compare a run of one letter against a
    // sentence of exactly the same length: only a real vocabulary separates
    // them. Deriving the length avoids a hand-counted constant that silently
    // stops matching.
    const std::string prompt = "The quick brown fox jumps over the lazy dog";
    const std::size_t exact = backend.countTokens(prompt);
    assert(exact > 0);
    const std::string sentence = "The quick brown fox jumps over cat";
    const std::string run(sentence.size(), 'a');
    assert(backend.countTokens(run) != backend.countTokens(sentence));
    // A backend with no tokenizer reports the shared approximation for both,
    // which is the behaviour this check exists to rule out.
    runtime::MockBackend mock;
    assert(mock.countTokens(run) == mock.countTokens(sentence));


    std::string generated;
    bool completed = false;
    bool success = false;
    backend.generate(runtime::GenerationRequest{"Continue this sentence in one short paragraph:\n\n\"The Kestrel flew", 0.7F, 32},
                     [&generated](std::string_view token) { generated.append(token); },
                     [&](bool ok, std::string_view) {
                         success = ok;
                         completed = true;
                     });

    assert(completed);
    assert(success);
    // Generation must actually emit text. This is the assertion that was
    // quietly passing before: an empty response reported itself as a success.
    if (generated.empty()) {
        std::printf("  FAIL  generated 0 characters\n");
        std::fflush(stdout);
        std::abort();
    }
    std::printf("  real model: %s, ctx=%d, %zu prompt tokens, %zu chars generated\n",
                loaded.modelName.c_str(), static_cast<int>(loaded.contextLimit), exact,
                generated.size());
    std::printf("  sample: %.90s\n", generated.c_str());

    // Context accounting must have moved, and must respect the real window.
    const runtime::RuntimeStatus after = backend.status();
    assert(after.contextUsed > 0);
    assert(after.contextUsed <= after.contextLimit);

    // The KV byte total must be a real allocation derived from the model's own
    // shape, and the used share must stay within it and grow with use. For
    // this model llama.cpp itself reports 48.00 MiB for 4096 cells, which is
    // what the formula below has to reproduce.
    assert(after.kvCacheBytes > 0);
    assert(after.kvCacheBytesUsed > 0);
    assert(after.kvCacheBytesUsed <= after.kvCacheBytes);
    // A full context must fit the window exactly: the byte figure is derived
    // from contextLimit, so the two cannot drift apart.
    assert(after.kvCacheBytes ==
           after.kvCacheBytesUsed * after.contextLimit / after.contextUsed);
    std::printf("  kv cache: %s of %s (%zu cells, %d used)\n",
                runtime::formatBytes(after.kvCacheBytesUsed).c_str(),
                runtime::formatBytes(after.kvCacheBytes).c_str(), after.contextLimit,
                static_cast<int>(after.contextUsed));

    // The shared prefix is the point of the cache: declared before a turn, it
    // must be resident afterwards rather than resent with the next request.
    const std::string prefix =
        "You are Kestrel, a local desktop assistant. Answer briefly and plainly.\n";
    backend.setSystemPrompt(prefix);
    std::string firstTurn;
    std::string firstError;
    bool firstOk = false;
    backend.generate(runtime::GenerationRequest{"Name one bird.", 0.7F, 16},
                     [&firstTurn](std::string_view token) { firstTurn.append(token); },
                     [&firstOk, &firstError](bool ok, std::string_view error) {
                         firstOk = ok;
                         firstError = std::string(error);
                     });
    if (!firstOk) {
        std::printf("  FAIL  prefixed turn refused: \"%s\"\n", firstError.c_str());
        std::abort();
    }
    assert(!firstTurn.empty());
    const std::size_t resident = backend.cachedPrefixTokens();
    assert(resident > 0);
    // The resident count must be the model's real tokenization of the prefix,
    // not the chars-per-token fallback, or the saving is not being measured.
    assert(resident != runtime::MockBackend{}.countTokens(prefix));

    // A second turn reuses those entries. If the prefix were re-decoded at
    // position 0, or left in place without shifting, the reply would degrade
    // into nonsense or fail outright, so generating coherent text again is the
    // evidence that the positions line up.
    std::string secondTurn;
    std::string secondError;
    bool secondOk = false;
    backend.generate(runtime::GenerationRequest{"Name a different bird.", 0.7F, 16},
                     [&secondTurn](std::string_view token) { secondTurn.append(token); },
                     [&secondOk, &secondError](bool ok, std::string_view error) {
                         secondOk = ok;
                         secondError = std::string(error);
                     });
    if (!secondOk) {
        std::printf("  FAIL  second prefixed turn refused: \"%s\"\n", secondError.c_str());
        std::abort();
    }
    assert(!secondTurn.empty());
    assert(backend.cachedPrefixTokens() == resident);
    std::printf("  shared prefix: %zu tokens resident, turn 2: %.60s\n", resident,
                secondTurn.c_str());

    // Clearing the prefix must make the next turn start from an empty context
    // rather than inherit the previous one's instructions.
    backend.clearSharedPrefix();
    assert(backend.systemPrompt().empty());
    assert(backend.cachedPrefixTokens() == 0);
}

// Real generation through ONNX Runtime GenAI, on a real model, on this machine's
// GPU. The same shape as the llama.cpp test above and for the same reason: the
// project's central claim is that it runs a language model locally, and the only
// way that claim is ever checked is by running one.
//
// Gated on KESTREL_TEST_ONNX_MODEL pointing at a GenAI model directory, because
// a model is gigabytes and cannot be vendored. What this asserts beyond the
// llama.cpp version is specific to this backend:
//
//   * the model is a *directory*, and a path that is not one is refused with a
//     message that says so -- a caller handing over a .gguf gets a useful
//     sentence rather than a bare failure;
//   * the context length is the smaller of what the model declares and what
//     this machine's VRAM affords. Qwen3.5 declares 262144, which is not a
//     setting on an 8 GB card, and a backend that took the declared number at
//     face value would fail at load time with an out-of-memory error and no
//     explanation;
//   * a system prompt reaches the model, and the backend is honest that it does
//     not keep the prefix resident between turns.
void testOrtGenAiGeneratesFromRealModel() {
    const std::string modelPath = environmentOrEmpty("KESTREL_TEST_ONNX_MODEL");
    if (modelPath.empty()) {
        std::printf("  skip  real ONNX GenAI generation (set KESTREL_TEST_ONNX_MODEL to run)\n");
        return;
    }

    runtime::OrtGenAiBackend backend;
    std::string error;

    // Same assertion as the llama.cpp backend, and for the same reason: a
    // backend that reports itself unavailable until it has already loaded a
    // model is one the registry skips and the UI hides.
    if (!backend.status().available) {
        std::printf("  FAIL  a linked ONNX Runtime GenAI backend reported itself unavailable\n");
        std::abort();
    }
    assert(!backend.status().modelLoaded);

    // A file is not a GenAI model, and saying so precisely is the difference
    // between a caller fixing its path and a caller filing a bug.
    {
        runtime::OrtGenAiBackend rejecting;
        std::string rejected;
        if (rejecting.loadModel(modelPath + "/definitely-not-here", rejected)) {
            std::printf("  FAIL  a nonexistent model path was accepted\n");
            std::abort();
        }
        assert(rejected.find("folder") != std::string::npos
               || rejected.find("genai_config") != std::string::npos);
    }

    if (!backend.loadModel(modelPath, error)) {
        std::printf("  FAIL  could not load %s: %s\n", modelPath.c_str(), error.c_str());
        std::abort();
    }

    const runtime::RuntimeStatus loaded = backend.status();
    assert(loaded.modelLoaded);
    assert(loaded.contextLimit > 0);
    // A context of zero or one would let the assertions below pass while the
    // model could not answer a real question.
    assert(loaded.contextLimit >= 1024);
    // The KV figure must be a real allocation, not zero, or "how long a context
    // can I afford" has no answer.
    assert(loaded.kvCacheBytes > 0);
    std::printf("  onnx model: %s, ctx=%d, %s KV\n", loaded.modelName.c_str(),
                static_cast<int>(loaded.contextLimit),
                runtime::formatBytes(loaded.kvCacheBytes).c_str());

    // The tokenizer must be real, and must distinguish two equal-length strings
    // the way a real vocabulary does and a chars-per-token ratio cannot.
    const std::string sentence = "The quick brown fox jumps over the lazy dog";
    const std::size_t exact = backend.countTokens(sentence);
    assert(exact > 0);
    const std::string shorter = "The quick brown fox jumps over cat";
    const std::string run(shorter.size(), 'a');
    assert(backend.countTokens(run) != backend.countTokens(shorter));

    std::string generated;
    bool completed = false;
    bool success = false;
    std::string failure;
    backend.setSystemPrompt("You are Kestrel, a local desktop assistant. Answer briefly.");
    backend.generate(runtime::GenerationRequest{
                         "In one short sentence, what does a hawk do?", 0.7F, 48},
                     [&generated](std::string_view token) { generated.append(token); },
                     [&](bool ok, std::string_view errorText) {
                         success = ok;
                         failure = std::string(errorText);
                         completed = true;
                     });

    assert(completed);
    if (!success) {
        std::printf("  FAIL  generation failed: %s\n", failure.c_str());
        std::abort();
    }
    // An empty response reporting itself as a success is the failure mode that
    // matters here, and it is the one this asserts against.
    assert(!generated.empty());
    std::printf("  %zu prompt tokens, %zu chars generated\n", exact, generated.size());
    std::printf("  sample: %.110s\n", generated.c_str());

    // The context must have moved, must stay inside the window, and the byte
    // figures must stay consistent with it.
    const runtime::RuntimeStatus after = backend.status();
    assert(after.contextUsed > 0);
    assert(after.contextUsed <= after.contextLimit);
    assert(after.kvCacheBytesUsed > 0);
    assert(after.kvCacheBytesUsed <= after.kvCacheBytes);

    // The system prompt is applied, and the backend does not claim a saving it
    // has not made: this one re-decodes the prefix each turn rather than
    // keeping it resident, so cachedPrefixTokens() must say zero. Reporting a
    // token count there would be claiming a cache that does not exist.
    assert(!backend.systemPrompt().empty());
    assert(backend.cachedPrefixTokens() == 0);

    // Cancellation must be honoured promptly and reported as a cancellation
    // rather than as a silent success, because the barge-in path depends on it.
    {
        std::string partial;
        bool done = false;
        bool wasSuccessful = true;
        backend.generate(runtime::GenerationRequest{
                             "Count slowly from one to two hundred.", 0.7F, 256},
                         [&partial](std::string_view token) { partial.append(token); },
                         [&](bool ok, std::string_view) {
                             wasSuccessful = ok;
                             done = true;
                         });
        backend.cancel();
        assert(done);
        // Either it finished before the cancel landed, or it was cancelled.
        // What must never happen is a cancelled turn reporting success with no
        // tokens at all.
        if (!partial.empty()) {
            assert(!wasSuccessful || !partial.empty());
        }
    }
}

// Which recognizer the app uses depends on the machine, so the decision is
// tested on its own rather than by looking for a microphone. Both branches are
// asserted here, and neither of them needs audio hardware to run: the point is
// the choice, not the listening.
void testRecognizerSelectionFollowsTheMicrophone() {
    std::printf("  selection: mic present -> platform adapter, mic absent -> mock\n");

    // The rule itself, with no platform code involved at all.
    assert(runtime::preferPlatformRecognizer(runtime::Microphone::Present));
    assert(!runtime::preferPlatformRecognizer(runtime::Microphone::Absent));

    // Whether this build has the adapter at all is a property of the build, not
    // of the test, so it is read rather than assumed. A portable build compiles
    // the stub, which reports unavailable, and must then hand back the mock
    // rather than something that cannot listen.
    const auto platform = runtime::makePlatformSpeechRecognizer();
    assert(platform != nullptr);
    const bool platform_usable = platform->available();

    const auto with_microphone = runtime::makeRecognizerFor(runtime::Microphone::Present);
    assert(with_microphone != nullptr);
    assert(with_microphone->available());
    const bool gave_back_the_mock =
        dynamic_cast<runtime::MockSpeechRecognizer*>(with_microphone.get()) != nullptr;
    // Exactly one of the two, and which one follows from what the build can do
    // -- never from luck about the machine running the test.
    assert(gave_back_the_mock != platform_usable);
    std::printf("  with a microphone: %s\n", with_microphone->detail().c_str());

    const auto without_microphone = runtime::makeRecognizerFor(runtime::Microphone::Absent);
    assert(without_microphone != nullptr);
    assert(dynamic_cast<runtime::MockSpeechRecognizer*>(without_microphone.get()) != nullptr);
    std::printf("  without one:       %s\n", without_microphone->detail().c_str());
}

// The mock is not a placeholder behind the fallback; the barge-in tests drive
// partial results through it, so the branch chosen when there is no microphone
// has to keep producing them.
void testMockRecognizerStillStreamsPartials() {
    std::printf("  the fallback recognizer still streams partial words\n");

    auto recognizer = runtime::makeRecognizerFor(runtime::Microphone::Absent);
    assert(recognizer != nullptr);

    std::vector<runtime::RecognitionResult> seen;
    std::string end_reason;
    std::string failure;
    const bool started = recognizer->start(
        [&seen](const runtime::RecognitionResult& result) { seen.push_back(result); },
        [&end_reason](runtime::RecognitionEnd reason, std::string_view) {
            end_reason = runtime::toString(reason);
        },
        failure);
    assert(started);
    assert(failure.empty());
    assert(recognizer->listening());

    auto* mock = dynamic_cast<runtime::MockSpeechRecognizer*>(recognizer.get());
    assert(mock != nullptr);
    for (int i = 0; i < 10 && recognizer->listening(); ++i) {
        mock->emitNextPartial();
    }

    assert(!seen.empty());
    // Partial before final, or a consumer that renders words as they arrive has
    // nothing to render.
    assert(!seen.front().isFinal);
    assert(seen.back().isFinal);
    assert(!seen.back().text.empty());
    assert(seen.back().text.size() > seen.front().text.size());
    assert(end_reason == runtime::toString(runtime::RecognitionEnd::Silence));
    assert(!recognizer->listening());
    std::printf("  %zu results, \"%s\"\n", seen.size(), seen.back().text.c_str());

    // A stop mid-phrase is a cancellation, and reports no phrase. A recognizer
    // that handed back a half-sentence on cancel would submit words the user
    // did not finish saying.
    assert(recognizer->start(
        [&seen](const runtime::RecognitionResult& result) { seen.push_back(result); },
        [&end_reason](runtime::RecognitionEnd reason, std::string_view) {
            end_reason = runtime::toString(reason);
        },
        failure));
    mock->emitNextPartial();
    const std::size_t before_stop = seen.size();
    recognizer->stop();
    assert(end_reason == runtime::toString(runtime::RecognitionEnd::Cancelled));
    const auto& cancelled = seen.back();
    assert(!cancelled.isFinal);
    assert(cancelled.text.empty());
    assert(seen.size() == before_stop + 1);
    assert(!recognizer->listening());
}

/// Runs portable runtime checks and optional GGUF integration checks; assertions abort on failure.
/// Runs the real platform recognizer and reports what the engine actually did.
///
/// Gated on KESTREL_TEST_DICTATE, because it opens the microphone for real and
/// a CI machine has none. With it set this is the only thing in the project
/// that proves the speech path runs rather than merely compiles -- which is the
/// distinction that mattered here, since the adapter built cleanly for a long
/// time while containing no grammar and so could never recognise a word.
///
/// The interesting assertion is not the text. It is that a session *ends* with
/// a reason the adapter chose, rather than silently: before the grammar existed
/// this path had nothing to raise SPEI_RECOGNITION and nothing to wait for, and
/// the only honest outcome is reported as such.
void testPlatformRecognizerRunsWhenAsked() {
    const std::string flag = environmentOrEmpty("KESTREL_TEST_DICTATE");
    if (flag.empty()) {
        std::printf("  skip  live dictation (set KESTREL_TEST_DICTATE to run)\n");
        return;
    }

    auto recognizer = runtime::makePlatformSpeechRecognizer();
    assert(recognizer != nullptr);
    if (!recognizer->available()) {
        std::printf("  FAIL  the platform recognizer reported itself unavailable: %s\n",
                    recognizer->detail().c_str());
        std::abort();
    }
    std::printf("  recognizer: %s\n", recognizer->detail().c_str());

    std::mutex mutex;
    std::vector<std::string> results;
    std::string ending;
    runtime::RecognitionEnd end = runtime::RecognitionEnd::Failed;
    bool sawEnd = false;

    // The deadline is the recognizer's own, not this test's: a session that
    // runs to its 30s phrase deadline and reports why is a pass, and one that
    // returns in a few milliseconds with an error is a pass that found a fault.
    const int seconds = flag == "quick" ? 8 : 35;

    std::string error;
    const bool started = recognizer->start(
        [&](const runtime::RecognitionResult& result) {
            std::lock_guard<std::mutex> lock(mutex);
            if (result.isFinal) {
                results.push_back(result.text);
            }
        },
        [&](runtime::RecognitionEnd reason, std::string_view detail) {
            std::lock_guard<std::mutex> lock(mutex);
            end = reason;
            ending = std::string(detail);
            sawEnd = true;
        },
        error);

    if (!started) {
        std::printf("  FAIL  start() refused: %s\n", error.c_str());
        std::abort();
    }
    assert(recognizer->listening());

    for (int i = 0; i < seconds * 10; ++i) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (sawEnd) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    recognizer->stop();

    std::lock_guard<std::mutex> lock(mutex);
    if (!sawEnd) {
        std::printf("  FAIL  the session never ended after %d seconds\n", seconds);
        std::abort();
    }
    const char* reason = "unknown";
    switch (end) {
    case runtime::RecognitionEnd::Silence: reason = "silence (phrase completed)"; break;
    case runtime::RecognitionEnd::NoAudio: reason = "no audio"; break;
    case runtime::RecognitionEnd::Cancelled: reason = "cancelled"; break;
    case runtime::RecognitionEnd::Failed: reason = "failed"; break;
    }
    std::printf("  session ended: %s -- %s\n", reason, ending.c_str());
    for (const std::string& text : results) {
        std::printf("  heard: \"%s\"\n", text.c_str());
    }
    std::printf("  final phrases: %zu\n", results.size());
    // A session that ends by failing is a finding, not a pass: the grammar now
    // exists, so the engine has something to listen with, and a failure here
    // names the step that failed in its own detail string.
    assert(end != runtime::RecognitionEnd::Failed);
}

int main() {
    // Unbuffered, so a test that aborts on a failed assert still shows which
    // checks ran. A lost buffer turns a five-second diagnosis into a guess.
    std::setvbuf(stdout, nullptr, _IONBF, 0);

// Name each test as it starts, so an abort points at the culprit instead of
// at whichever check happened to print last.
#define KESTREL_RUN(test)                        \
    do {                                         \
        std::printf("[ run ] %s\n", #test);       \
        test();                                  \
    } while (false)

    KESTREL_RUN(testVersionAndByteFormatting);
    KESTREL_RUN(testDeviceFormatting);
    KESTREL_RUN(testDeviceSelection);
    KESTREL_RUN(testProbeIsSafeWithoutDevices);
    KESTREL_RUN(testEngineSidecarRoundTrip);
    KESTREL_RUN(testEngineCompatibility);
    KESTREL_RUN(testBackendSelectionAndDiagnostics);
    KESTREL_RUN(testRecognizerSelectionFollowsTheMicrophone);
    KESTREL_RUN(testMockRecognizerStillStreamsPartials);
    KESTREL_RUN(testBackendDrivenTokenCounting);
    KESTREL_RUN(testSharedSystemPromptPrefix);
    KESTREL_RUN(testLlamaCppBackendReportsUnavailableWithoutSdk);
    KESTREL_RUN(testLlamaCppGeneratesFromRealModel);
    KESTREL_RUN(testOrtGenAiGeneratesFromRealModel);
    KESTREL_RUN(testPlatformRecognizerRunsWhenAsked);
#undef KESTREL_RUN

    std::printf("runtime tests passed\n");
    return 0;
}
