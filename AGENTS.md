# Kestrel contributor guidance

## Project overview

Kestrel is a Windows-first, local personal AI desktop agent. The portable core is
C++20 and CMake; the desktop shell is Qt 6/QML and is optional at configure time.

## Repository map

- `src/core/`: conversation and message domain state; keep this layer portable and
  independent of Qt and inference SDKs.
- `src/runtime/`: backend contracts and adapters. Keep native llama.cpp and
  TensorRT integration contained in their respective adapters.
- `src/app/`: Qt application startup and UI-facing controller.
- `src/ui/`: QML desktop interface.
- `tests/`: small, dependency-free tests for portable behavior.

## Implementation conventions

- Target C++20 and preserve the repository's existing style: four-space
  indentation, braces on the same line as declarations, and `kestrel::<layer>`
  namespaces.
- Prefer standard-library types in `core` and `runtime`; do not let Qt types or
  UI concerns cross into portable backend/domain interfaces.
- Make behavior changes testable. Add or update coverage in `tests/core_tests.cpp`
  when modifying core or mock-backend behavior.
- Keep model-specific code behind `ModelBackend`; the mock backend must remain
  usable without external model runtimes.
- Do not commit generated build directories or local IDE/cache artifacts.

## Build and test

Core-only verification (works without Qt):

```powershell
cmake -S . -B build -DKESTREL_BUILD_UI=OFF
cmake --build build --config Debug
ctest --test-dir build -C Debug --output-on-failure
```

For UI changes, configure with `-DKESTREL_BUILD_UI=ON`. Qt 6.6+ is required for
the desktop target; CMake will skip that target when Qt is unavailable.
