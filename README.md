# Kestrel

Kestrel is a local-first personal AI desktop agent with a quiet, focused interface and GPU-accelerated inference.

The first target is **Windows + NVIDIA**, with a C++20 core, CMake build, Qt 6/QML interface, and interchangeable inference backends:

- **llama.cpp** for practical GGUF model support and CUDA offload.
- **TensorRT** as an NVIDIA-optimized backend boundary.
- A built-in mock backend so the UI and application shell can be developed without a model installed.

## Product direction

Kestrel should feel like a soft glass command center rather than a busy dashboard: focused chat, a collapsible conversation sidebar, generous spacing, restrained color, smooth transitions, and useful runtime details that stay out of the way.

## Repository layout

```text
src/
  app/       Qt application controller and entry point
  core/      Conversations and message state
  runtime/   Backend interface and runtime adapters
  ui/        QML desktop shell
tests/       Small dependency-free core tests
```

## Requirements

### Core and tests

- CMake 3.24+
- A C++20 compiler

### Desktop UI

- Qt 6.6+ with Quick, QuickControls2, and QML

The UI target is optional at configure time. If Qt 6 is unavailable, the core library and tests still build.

### Inference backends

The initial adapters define the integration boundary without bundling third-party runtimes. Add llama.cpp and TensorRT through your preferred package/build strategy, then connect their native calls inside the corresponding adapter files.

## Build

```powershell
cmake -S . -B build -DKESTREL_BUILD_UI=ON
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

To build only the portable core and tests:

```powershell
cmake -S . -B build -DKESTREL_BUILD_UI=OFF
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

## Current milestone

This scaffold includes the application architecture, backend contracts, a mock streaming chat flow, and the first soft-glass QML workspace. Model loading, persistence, and native llama.cpp/TensorRT calls are intentionally isolated for the next milestone.
