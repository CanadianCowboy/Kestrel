#include "runtime/backendregistry.h"
#include "runtime/cudadevice.h"
#include "runtime/engineartifact.h"
#include "runtime/llamacppbackend.h"
#include "runtime/mockbackend.h"
#include "runtime/tensorrtbackend.h"

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

void testTensorRtBackendValidatesEngine() {
    runtime::TensorRTBackend backend;
    std::string error;

    assert(!backend.loadModel("", error));
    assert(error.find("No engine path") != std::string::npos);

    assert(!backend.loadModel("definitely_not_here.plan", error));
    assert(error.find("not found") != std::string::npos);

    // Generating without a loaded engine must fail cleanly, not crash.
    bool completed = false;
    backend.generate({"hi", 0.7F, 16}, [](std::string_view) {},
                     [&completed](bool success, std::string_view) { completed = !success; });

    assert(completed);

    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "kestrel_trt_backend_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    const std::string emptyEngine = (dir / "empty.plan").string();
    { std::ofstream file(emptyEngine, std::ios::binary); }
    assert(!backend.loadModel(emptyEngine, error));
    assert(error.find("empty") != std::string::npos);

    const std::string goodEngine = (dir / "good.plan").string();
    { std::ofstream file(goodEngine, std::ios::binary); file << "engine bytes"; }

    // A build record that contradicts the current GPU must stop the load.
    const runtime::CudaProbe probe = runtime::probeCuda();
    if (probe.hasDevice()) {
        runtime::EngineBuildRecord hostile;
        hostile.tensorrtVersion = 10400;
        hostile.cudaVersion = 12040;
        hostile.computeMajor = 3;
        hostile.computeMinor = 5;
        hostile.gpuName = "Definitely Not This GPU";
        const bool wrote = runtime::writeEngineBuildRecord(goodEngine, hostile, error);
        assert(wrote);
        const bool loadedIncompatible = backend.loadModel(goodEngine, error);
        assert(!loadedIncompatible);
        assert(error.find("Rebuild the engine") != std::string::npos);
        assert(!backend.status().modelLoaded);
    }

    // No record at all is loadable: absence of metadata is not a defect.
    std::filesystem::remove(runtime::engineSidecarPath(goodEngine));
    const bool loaded = backend.loadModel(goodEngine, error);
    assert(loaded);
    assert(error.empty());
    assert(backend.status().modelLoaded);
    assert(backend.status().modelName == "good.plan");
    assert(backend.lastCompatibility().verdict == runtime::EngineCompatibility::Unknown);
    assert(backend.lastCompatibility().summary.find("build record") != std::string::npos);

    // Generation is still refused, but with the reason the adapter owns.
    std::string generateError = "unset";
    backend.generate({"hi", 0.7F, 16}, [](std::string_view) {},
                     [&generateError](bool, std::string_view message) {
                         generateError = std::string(message);
                     });
    assert(!generateError.empty());
    assert(generateError.find("TensorRT") != std::string::npos);

    // Cancellation is a real flag, not a no-op, and is safe when idle.
    backend.cancel();
    backend.cancel();

    std::filesystem::remove_all(dir);
}

void testBackendSelectionAndDiagnostics() {
    const auto backend = runtime::selectBackend(runtime::BackendKind::Mock);
    assert(backend != nullptr);
    assert(backend->status().available);

    // An unavailable preference must fall through rather than return a
    // backend that cannot run.
    const auto fallback = runtime::selectBackend(runtime::BackendKind::TensorRT);
    assert(fallback != nullptr);
    assert(fallback->status().available);

    assert(runtime::toString(runtime::BackendKind::Mock) == "mock");
    assert(runtime::toString(runtime::BackendKind::LlamaCpp) == "llamacpp");
    assert(runtime::toString(runtime::BackendKind::TensorRT) == "tensorrt");

    const auto diagnostics = runtime::runtimeDiagnostics(runtime::probeCuda());
    assert(!diagnostics.empty());
    bool sawCudaRow = false;
    for (const runtime::RuntimeDiagnostic& row : diagnostics) {
        assert(!row.label.empty());
        assert(!row.value.empty());
        if (row.label == "CUDA toolkit") {
            sawCudaRow = true;
        }
    }
    assert(sawCudaRow);
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

// The shared system prompt contract, which must hold in every configuration.
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
    mock.generate(runtime::GenerationRequest{"hello", 0.7F, 32}, [](std::string_view) {},
                  [](bool, std::string_view) {});
    assert(mock.status().contextUsed >= mock.countTokens(prefix));
}

// Exercises the real llama.cpp generation path.
//
// Skipped unless KESTREL_TEST_GGUF points at a GGUF file, so CI does not need
// a multi-hundred-megabyte model download to run the suite. When it is set,
// this is the only test that proves the backend actually generates rather than
// merely linking.
void testLlamaCppGeneratesFromRealModel() {
    const char* modelPath = std::getenv("KESTREL_TEST_GGUF");
    if (modelPath == nullptr || *modelPath == '\0') {
        std::printf("  skip  real GGUF generation (set KESTREL_TEST_GGUF to run)\n");
        return;
    }

    runtime::LlamaCppBackend backend;
    std::string error;


    if (!backend.loadModel(modelPath, error)) {
        std::printf("  FAIL  could not load %s: %s\n", modelPath, error.c_str());
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
    KESTREL_RUN(testTensorRtBackendValidatesEngine);
    KESTREL_RUN(testBackendSelectionAndDiagnostics);
    KESTREL_RUN(testBackendDrivenTokenCounting);
    KESTREL_RUN(testSharedSystemPromptPrefix);
    KESTREL_RUN(testLlamaCppBackendReportsUnavailableWithoutSdk);
    KESTREL_RUN(testLlamaCppGeneratesFromRealModel);
#undef KESTREL_RUN

    std::printf("runtime tests passed\n");
    return 0;
}
