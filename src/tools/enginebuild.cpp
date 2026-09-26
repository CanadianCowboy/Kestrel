// Offline engine-build tool.
//
// Converts a model into a TensorRT engine and writes the sidecar build record
// that Kestrel's validation path reads. It is deliberately a separate
// executable rather than a mode of the desktop app: building an engine is a
// long, machine-specific operation, and the app must never write to engines.
//
// The record is written from facts probed on the machine that runs this tool,
// never from values supplied on the command line. A build record that does
// not describe reality is worse than no record at all, because validation
// would then confidently wave a mismatched engine through.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "runtime/cudadevice.h"
#include "runtime/engineartifact.h"

#if defined(KESTREL_TENSORRT_ENABLED) && KESTREL_TENSORRT_ENABLED
#define KESTREL_ENGINEBUILD_HAS_TENSORRT 1
#include <NvInfer.h>
#include <NvInferRuntime.h>
#include <NvInferVersion.h>
#include <NvOnnxParser.h>
#endif

namespace {

using kestrel::runtime::CudaDeviceInfo;
using kestrel::runtime::CudaProbe;
using kestrel::runtime::EngineBuildRecord;

void printUsage() {
    std::cout
        << "kestrel-engine-build - build a TensorRT engine and its Kestrel build record\n\n"
           "Usage:\n"
           "  kestrel-engine-build --model <model.onnx> --output <engine.plan>\n"
           "  kestrel-engine-build --engine <engine.plan> --record-only\n\n"
           "Options:\n"
           "  --model <path>     Model to convert. Requires a TensorRT SDK at build time.\n"
           "  --output <path>    Destination engine file. Defaults to <model>.plan.\n"
           "  --engine <path>    Existing engine to annotate. Used with --record-only.\n"
           "  --record-only      Write only the build record for this machine. No conversion.\n"
           "  -h, --help         Show this message.\n\n"
           "The build record is always written from the GPU and CUDA actually\n"
           "present on this machine.\n";
}

[[nodiscard]] std::string valueAfter(int argc, char** argv, int& index) {
    if (index + 1 >= argc) {
        return {};
    }
    return argv[++index];
}

#if defined(KESTREL_ENGINEBUILD_HAS_TENSORRT)

// TensorRT logs through a caller-supplied sink. Route it to stderr so build
// diagnostics are not silently swallowed.
class StderrLogger final : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* message) noexcept override {
        if (severity <= Severity::kWARNING) {
            std::cerr << "[TensorRT] " << (message != nullptr ? message : "") << "\n";
        }
    }
};

StderrLogger g_logger;

// Returns false and fills `error` when the model cannot be parsed.
[[nodiscard]] bool buildEngine(const std::string& modelPath,
                               const std::string& enginePath,
                               int computeMajor,
                               std::string& error) {
    nvinfer1::IBuilder* builder = nvinfer1::createInferBuilder(g_logger);
    if (builder == nullptr) {
        error = "TensorRT could not create a builder. Is a GPU visible to this process?";
        return false;
    }

    std::unique_ptr<nvinfer1::INetworkDefinition> network{builder->createNetworkV2(0)};
    std::unique_ptr<nvonnxparser::IParser> parser{
        nvonnxparser::createParser(*network, g_logger)};
    if (!parser->parseFromFile(modelPath.c_str(), static_cast<int>(modelPath.size()))) {
        error = "TensorRT could not parse " + modelPath + ": " + parser->getError();
        return false;
    }

    auto config = builder->createBuilderConfig();
    // An engine is only loadable by the exact GPU architecture it was built
    // for, so target the capability probed on this machine rather than a
    // default, and record that same capability in the sidecar below.
    config->setFlag(nvinfer1::BuilderFlag::kFP16);
    if (computeMajor >= 8) {
        config->setFlag(nvinfer1::BuilderFlag::kFP8);
    }

    std::unique_ptr<nvinfer1::ICudaEngine> engine{
        builder->buildSerializedNetwork(*network, *config)};
    if (engine == nullptr) {
        error = "TensorRT failed to build an engine. Check free GPU memory and try again.";
        return false;
    }

    std::ofstream out(enginePath, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Could not open " + enginePath + " for writing.";
        return false;
    }
    out.write(reinterpret_cast<const char*>(engine->data()),
              static_cast<std::streamsize>(engine->size()));
    if (!out) {
        error = "Failed while writing " + enginePath + ".";
        return false;
    }
    return true;
}

#endif // KESTREL_ENGINEBUILD_HAS_TENSORRT

} // namespace

int main(int argc, char** argv) {
    std::string modelPath;
    std::string enginePath;
    std::string recordOnlyEngine;
    bool recordOnly = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        }
        if (arg == "--model") {
            modelPath = valueAfter(argc, argv, i);
        } else if (arg == "--output") {
            enginePath = valueAfter(argc, argv, i);
        } else if (arg == "--engine") {
            recordOnlyEngine = valueAfter(argc, argv, i);
        } else if (arg == "--record-only") {
            recordOnly = true;
        } else {
            std::cerr << "Unrecognized argument: " << arg << "\n\n";
            printUsage();
            return 2;
        }
    }

    // Decide which engine file the record describes.
    if (recordOnly) {
        enginePath = recordOnlyEngine;
        if (enginePath.empty()) {
            std::cerr << "--record-only requires --engine <engine.plan>.\n";
            return 2;
        }
    } else {
        if (modelPath.empty()) {
            std::cerr << "--model is required. Run with --help for usage.\n";
            return 2;
        }
        if (enginePath.empty()) {
            enginePath = modelPath + ".plan";
        }
    }

    // Probe before doing anything expensive, so an unusable machine fails fast
    // with a clear reason instead of partway through a build.
    const CudaProbe probe = kestrel::runtime::probeCuda();
    const CudaDeviceInfo* device = probe.selectedDevice();
    if (device == nullptr) {
        std::cerr << "No usable CUDA device: " << probe.summary() << "\n"
                  << "A build record must describe a real GPU, so there is nothing to write.\n";
        return 1;
    }

    if (!recordOnly) {
#if defined(KESTREL_ENGINEBUILD_HAS_TENSORRT)
        std::cout << "Building " << enginePath << " from " << modelPath << "...\n";
        std::string error;
        if (!buildEngine(modelPath, enginePath, device->computeMajor, error)) {
            std::cerr << error << "\n";
            return 1;
        }
#else
        std::cerr << "This build has no TensorRT SDK linked, so it cannot convert "
                     "a model into an engine.\n"
                  << "Configure with -DKESTREL_ENABLE_TENSORRT=ON and "
                     "-DKESTREL_TENSORRT_ROOT=<sdk> and rebuild.\n"
                  << "Use --record-only to write a build record for an engine that "
                     "was built elsewhere.\n";
        return 1;
#endif
    } else {
        if (!std::filesystem::exists(enginePath)) {
            std::cerr << "No such engine file: " << enginePath
                      << "\n--record-only annotates an existing engine; it does not create one.\n";
            return 1;
        }
    }

    // Only now write the record, and only from probed facts.
    EngineBuildRecord record;
#if defined(KESTREL_ENGINEBUILD_HAS_TENSORRT)
    record.tensorrtVersion = NV_TENSORRT_MAJOR * 1000 + NV_TENSORRT_MINOR * 100 + NV_TENSORRT_PATCH;
#endif
    record.cudaVersion = probe.runtime.runtimeVersion;
    record.computeMajor = device->computeMajor;
    record.computeMinor = device->computeMinor;
    record.gpuName = device->name;
    record.builtBy = "kestrel-engine-build";

    std::string error;
    if (!kestrel::runtime::writeEngineBuildRecord(enginePath, record, error)) {
        std::cerr << error << "\n";
        return 1;
    }

    std::cout << "Wrote " << kestrel::runtime::engineSidecarPath(enginePath) << "\n"
              << "  GPU    : " << record.gpuName << " (sm_"
              << record.computeMajor << record.computeMinor << ")\n"
              << "  CUDA   : " << kestrel::runtime::formatCudaVersion(probe.runtime.runtimeVersion)
              << "\n"
              << "  TRT    : "
              << (record.tensorrtVersion > 0
                      ? kestrel::runtime::formatTensorRTVersion(record.tensorrtVersion)
                      : std::string("not linked into this build"))
              << "\n";

    // Read it straight back so the tool proves the record parses rather than
    // assuming the write succeeded. This is what makes the validation path
    // exercisable end to end without a real engine present.
    EngineBuildRecord readBack;
    if (!kestrel::runtime::readEngineBuildRecord(enginePath, readBack, error)) {
        std::cerr << "Record did not round-trip: " << error << "\n";
        return 1;
    }

    const auto report = kestrel::runtime::checkEngineCompatibility(
        readBack, probe, record.tensorrtVersion);
    std::cout << "  Verify : " << report.summary << "\n"
              << "  Verdict: "
              << kestrel::runtime::EngineCompatibilityReport::toString(report.verdict) << "\n";
    if (!report.remedy.empty()) {
        std::cout << "  Remedy : " << report.remedy << "\n";
    }
    return 0;
}
