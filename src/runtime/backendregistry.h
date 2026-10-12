#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/cudadevice.h"
#include "runtime/modelbackend.h"

namespace kestrel::runtime {

// One line of runtime diagnostics for the UI and for bug reports.
struct RuntimeDiagnostic {
    std::string label;
    std::string value;
    bool ok = false;
};

// Stable lowercase identifier for a backend kind, e.g. "tensorrt".
[[nodiscard]] std::string_view toString(BackendKind kind) noexcept;

// Every backend Kestrel knows how to construct, in preference order, with
// status already resolved. Used for diagnostics; the instances are throwaway.
[[nodiscard]] std::vector<RuntimeDiagnostic> runtimeDiagnostics(const CudaProbe& probe);

// Picks a backend to run with.
//
// Returns an available local inference backend, preferring the requested kind
// and then trying the other real runtimes. Explicit Mock selection remains for
// tests only. If no inference runtime is linked, returns the unavailable
// llama.cpp adapter so callers receive an actionable runtime diagnostic rather
// than silently generating canned text.
[[nodiscard]] std::unique_ptr<ModelBackend> selectBackend(BackendKind preferred = BackendKind::LlamaCpp);

} // namespace kestrel::runtime
