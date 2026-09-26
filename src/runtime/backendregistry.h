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
// Returns the first constructed backend whose status reports available, so a
// machine with TensorRT linked uses it and a machine without falls back to the
// mock. `preferred` names a backend kind to try first; an unknown or empty
// value keeps the default order. Never returns null: the mock backend is
// always constructible, because a UI with no runtime still has to render.
[[nodiscard]] std::unique_ptr<ModelBackend> selectBackend(BackendKind preferred = BackendKind::Mock);

} // namespace kestrel::runtime
