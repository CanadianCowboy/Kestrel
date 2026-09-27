# Kestrel contributor guidance

## Project overview

Kestrel is a Windows-first, local personal AI desktop agent. The portable core is
C++20 and CMake; the desktop shell is Qt 6/QML and is optional at configure time.

## Repository map

- `src/core/`: conversation and message domain state, the assistant's personality
  (`persona.*`), the presence projection the UI animates on (`presence.*`), the
  sandboxed idle loop (`idlepersona.*`), and the voice response timeline
  (`voicesession.*`); keep this layer portable and independent of Qt and
  inference SDKs.
- `src/runtime/`: backend contracts and adapters. Keep native llama.cpp and
  TensorRT integration contained in their respective adapters.
- `src/app/`: Qt application startup and UI-facing controller.
- `src/ui/`: QML desktop interface.
- `tests/`: small, dependency-free tests for portable behavior.

## Vendor SDK boundary

`src/runtime/cudadiscovery_cuda.cpp` is the only translation unit that may
include a CUDA header, `src/runtime/tensorrtbackend.cpp` is the only one that
may include TensorRT headers, and `src/runtime/sapirecognizer_win32.cpp` is the
only one that may include a SAPI header. All other code, including
`cudadevice.h`, `sapirecognizer.h`, `AppController`, and QML, reads platform
facts through the portable structs and interfaces.

CMake picks `cudadiscovery_cuda.cpp` or `cudadiscovery_stub.cpp` based on
`KESTREL_ENABLE_CUDA` and toolkit detection, and `sapirecognizer_win32.cpp` or
`sapirecognizer_stub.cpp` based on the Windows SDK. Do not add a second
CUDA-including or SAPI-including file; extend the portable header and the
existing implementation instead.

## Implementation conventions

- Target C++20 and preserve the repository's existing style: four-space
  indentation, braces on the same line as declarations, and `kestrel::<layer>`
  namespaces.
- Prefer standard-library types in `core` and `runtime`; do not let Qt types or
  UI concerns cross into portable backend/domain interfaces.
- Make behavior changes testable. Add or update coverage in `tests/core_tests.cpp`
  for core or mock-backend behavior, and in `tests/runtime_tests.cpp` for device
  discovery, engine records, and backend selection.
- Keep model-specific code behind `ModelBackend`; the mock backend must remain
  usable without external model runtimes.
- Keep the idle loop local. `IdlePersona` may only produce strings, numbers, and
  enums, and may not gain a network, filesystem, or command capability. The one
  task that touches the GPU (`ModelWarmup`) stays opt-in. Anything that needs
  those capabilities is an agent tool with explicit permission, not idle work.
- Time is injected into `core` by its owner, never read from a clock. That is
  what keeps the presence easing and the idle cadence testable.
- Compile options are shared on purpose: `KESTREL_STRICT_FLAGS` in
  `CMakeLists.txt` is applied to the library and to the tests, because a test
  built more leniently than the code it covers can pass against a translation
  unit the shipping target would reject. `/utf-8` is part of that contract
  because the persona layer emits real ellipses and middots.
- Report failures as actionable messages that tell the user what to do next, not
  bare status codes. A missing GPU, an old driver, and a mismatched engine are
  different problems and need different advice.
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

CUDA device discovery compiles in automatically when a CUDA Toolkit is present
and falls back to a stub that explains itself when it is not, so no extra flag is
needed for normal work. To verify or disable it explicitly:

```powershell
cmake -S . -B build-nocuda -DKESTREL_BUILD_UI=OFF -DKESTREL_ENABLE_CUDA=OFF
```

`kestrel.exe --print-runtime` reports the detected device and every backend's
availability without opening a window. Use it in development notes instead of
recalling GPU, driver, and CUDA versions from memory.
