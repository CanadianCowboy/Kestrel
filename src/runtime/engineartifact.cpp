#include "runtime/engineartifact.h"

#include <fstream>
#include <sstream>
#include <string>
#include <utility>

namespace kestrel::runtime {

namespace {

constexpr std::string_view kSidecarSuffix = ".kestrel-engine";

std::string trim(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t')) {
        ++begin;
    }
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t' || text[end - 1] == '\r')) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

int parseInt(const std::string& value, int fallback) {
    try {
        std::size_t consumed = 0;
        const int parsed = std::stoi(value, &consumed);
        if (consumed != value.size()) {
            return fallback;
        }
        return parsed;
    } catch (...) {
        return fallback;
    }
}

void writeField(std::ostream& out, std::string_view key, const std::string& value) {
    // Keys are fixed and values come from tooling, but newlines would corrupt
    // the record, so normalise them away rather than trusting the input.
    std::string sanitised;
    sanitised.reserve(value.size());
    for (const char c : value) {
        sanitised += (c == '\n' || c == '\r') ? ' ' : c;
    }
    out << key << '=' << sanitised << '\n';
}

} // namespace

bool EngineCompatibilityReport::loadable() const noexcept {
    return verdict != EngineCompatibility::Incompatible;
}

std::string_view EngineCompatibilityReport::toString(EngineCompatibility verdict) noexcept {
    switch (verdict) {
    case EngineCompatibility::Compatible: return "compatible";
    case EngineCompatibility::CompatibleWithWarning: return "compatible-with-warning";
    case EngineCompatibility::Incompatible: return "incompatible";
    case EngineCompatibility::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view engineSidecarSuffix() noexcept {
    return kSidecarSuffix;
}

std::string engineSidecarPath(const std::string& enginePath) {
    return enginePath + std::string(kSidecarSuffix);
}

std::string formatTensorRTVersion(int version) {
    if (version <= 0) {
        return {};
    }
    // TensorRT packs NV_TENSORRT_VERSION as major*1000 + minor*100 + patch, so
    // the minor digit sits in the hundreds place, not the tens.
    const int major = version / 1000;
    const int minor = (version % 1000) / 100;
    const int patch = version % 100;
    std::string text = std::to_string(major) + "." + std::to_string(minor);
    if (patch != 0) {
        text += "." + std::to_string(patch);
    }
    return text;
}

int compareVersions(int lhs, int rhs) noexcept {
    if (lhs < rhs) {
        return -1;
    }
    if (lhs > rhs) {
        return 1;
    }
    return 0;
}

EngineCompatibilityReport checkEngineCompatibility(const EngineBuildRecord& record,
                                                    const CudaProbe& probe,
                                                    int tensorrtVersion) {
    EngineCompatibilityReport report;
    report.hasRecord = true;
    report.record = record;

    if (record.tensorrtVersion <= 0 && record.cudaVersion <= 0
        && record.computeMajor <= 0 && record.gpuName.empty()) {
        report.verdict = EngineCompatibility::Unknown;
        report.summary = "Engine build record is empty; nothing to verify against this machine.";
        return report;
    }

    const CudaDeviceInfo* device = probe.selectedDevice();

    // A mismatched compute capability is fatal: a TensorRT plan contains
    // kernels built for one SM architecture and cannot run on another.
    if (device != nullptr && record.computeMajor > 0
        && (record.computeMajor != device->computeMajor
            || record.computeMinor != device->computeMinor)) {
        report.verdict = EngineCompatibility::Incompatible;
        report.summary = "Engine was built for sm_"
            + std::to_string(record.computeMajor) + "." + std::to_string(record.computeMinor)
            + " but this machine reports sm_" + device->computeCapability() + ".";
        report.remedy = "Rebuild the engine for " + device->name
            + " (sm_" + device->computeCapability()
            + ") on this machine. TensorRT engines are not portable across GPU architectures.";
        return report;
    }

    if (probe.runtime.available && !probe.runtime.driverSufficient()
        && record.cudaVersion > 0) {
        report.verdict = EngineCompatibility::Incompatible;
        report.summary = "Engine was built against CUDA "
            + formatCudaVersion(record.cudaVersion) + " but the installed driver reports "
            + formatCudaVersion(probe.runtime.driverVersion) + ".";
        report.remedy = "Update the NVIDIA driver, or rebuild the engine against a CUDA "
                        "toolkit the installed driver supports.";
        return report;
    }

    if (tensorrtVersion > 0 && record.tensorrtVersion > 0
        && compareVersions(record.tensorrtVersion, tensorrtVersion) != 0) {
        report.verdict = EngineCompatibility::Incompatible;
        report.summary = "Engine was built with TensorRT "
            + formatTensorRTVersion(record.tensorrtVersion) + " but this build uses "
            + formatTensorRTVersion(tensorrtVersion) + ".";
        report.remedy = "TensorRT engines are version-locked. Rebuild the engine with TensorRT "
            + formatTensorRTVersion(tensorrtVersion) + ", or install a build of Kestrel that "
                                "embeds that TensorRT version.";
        return report;
    }

    if (tensorrtVersion <= 0 && record.tensorrtVersion > 0) {
        report.verdict = EngineCompatibility::CompatibleWithWarning;
        report.summary = "Engine was built with TensorRT "
            + formatTensorRTVersion(record.tensorrtVersion)
            + " for sm_" + std::to_string(record.computeMajor) + "."
            + std::to_string(record.computeMinor)
            + ", but this build embeds no TensorRT runtime to verify against.";
        report.remedy = "Configure with -DKESTREL_ENABLE_TENSORRT=ON to load this engine.";
        return report;
    }

    if (device != nullptr && !record.gpuName.empty() && record.gpuName != device->name) {
        report.verdict = EngineCompatibility::CompatibleWithWarning;
        report.summary = "Engine was built on \"" + record.gpuName + "\" but this machine reports \""
            + device->name + "\" with a matching compute capability.";
        report.remedy = "Engines are not portable across GPU models even at the same compute "
                        "capability. Rebuild the engine here if it fails to load.";
        return report;
    }

    report.verdict = EngineCompatibility::Compatible;
    report.summary = "Engine matches this machine (sm_"
        + std::to_string(record.computeMajor) + "." + std::to_string(record.computeMinor)
        + ", CUDA " + formatCudaVersion(record.cudaVersion) + ").";
    return report;
}

bool readEngineBuildRecord(const std::string& enginePath,
                           EngineBuildRecord& record,
                           std::string& error) {
    const std::string path = engineSidecarPath(enginePath);
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "No engine build record at " + path;
        return false;
    }

    EngineBuildRecord parsed;
    std::string line;
    while (std::getline(file, line)) {
        const std::string trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        const std::size_t separator = trimmed.find('=');
        if (separator == std::string::npos) {
            error = "Malformed line in " + path + ": " + trimmed;
            return false;
        }
        const std::string key = trim(std::string_view(trimmed).substr(0, separator));
        const std::string value = trim(std::string_view(trimmed).substr(separator + 1));
        if (key == "tensorrt_version") {
            parsed.tensorrtVersion = parseInt(value, 0);
        } else if (key == "cuda_version") {
            parsed.cudaVersion = parseInt(value, 0);
        } else if (key == "compute_major") {
            parsed.computeMajor = parseInt(value, 0);
        } else if (key == "compute_minor") {
            parsed.computeMinor = parseInt(value, 0);
        } else if (key == "gpu_name") {
            parsed.gpuName = value;
        } else if (key == "built_by") {
            parsed.builtBy = value;
        } else {
            error = "Unknown key \"" + key + "\" in " + path;
            return false;
        }
    }

    record = std::move(parsed);
    error.clear();
    return true;
}

bool writeEngineBuildRecord(const std::string& enginePath,
                            const EngineBuildRecord& record,
                            std::string& error) {
    const std::string path = engineSidecarPath(enginePath);
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "Could not write engine build record to " + path;
        return false;
    }

    file << "# Kestrel engine build record. Written by the offline engine-build step.\n";
    writeField(file, "tensorrt_version", std::to_string(record.tensorrtVersion));
    writeField(file, "cuda_version", std::to_string(record.cudaVersion));
    writeField(file, "compute_major", std::to_string(record.computeMajor));
    writeField(file, "compute_minor", std::to_string(record.computeMinor));
    writeField(file, "gpu_name", record.gpuName);
    writeField(file, "built_by", record.builtBy);

    if (!file) {
        error = "Failed while writing engine build record to " + path;
        return false;
    }
    error.clear();
    return true;
}

} // namespace kestrel::runtime
