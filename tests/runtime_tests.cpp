#include "runtime/backendregistry.h"
#include "runtime/cudadevice.h"
#include "runtime/engineartifact.h"
#include "runtime/mockbackend.h"
#include "runtime/tensorrtbackend.h"

#include <cassert>
#include <cstdio>
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

int main() {
    testVersionAndByteFormatting();
    testDeviceFormatting();
    testDeviceSelection();
    testProbeIsSafeWithoutDevices();
    testEngineSidecarRoundTrip();
    testEngineCompatibility();
    testTensorRtBackendValidatesEngine();
    testBackendSelectionAndDiagnostics();
    std::printf("runtime tests passed\n");
    return 0;
}
