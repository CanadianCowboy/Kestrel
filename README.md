# Kestrel

Kestrel is a local-first personal AI desktop application, with Windows and NVIDIA GPU support as its primary optimization target. It provides a private workspace for chatting with local models, persistent conversations, local speech options, and explicitly permissioned idle tools.

The project prioritizes:

- **Local execution and privacy:** prompts, responses, and future agent data should remain on the user's machine unless the user deliberately enables an external integration.
- **A polished, quiet interface:** Kestrel should feel like a streamlined command center rather than a crowded AI dashboard.
- **NVIDIA-first acceleration:** CUDA and NVIDIA's CUDA-X ecosystem are the primary optimization target.
- **A maintainable C++ foundation:** application state, runtime integration, and UI are separated so contributors can work independently.
- **Safe extensibility:** tools and automation must be explicit, inspectable, cancellable, and disabled by default until configured.

> **Project status:** Kestrel has a working Qt/QML desktop app, persistent local conversations, streaming generation, a model picker, runtime diagnostics, and opt-in local tools. Native inference is available through optional llama.cpp (GGUF) and ONNX Runtime GenAI integrations; a mock preview backend keeps dependency-light builds usable. Speech input and synthesis work through separate adapters, with important real-time voice capabilities still on the roadmap.

## Vision

Kestrel aims to become a personal local AI environment with three layers:

1. **Conversation:** a fast, private chat interface with streaming output, history, model controls, and useful context handling.
2. **Intelligence runtime:** local inference through pluggable runtimes, with CUDA acceleration where supported.
3. **Agent capabilities:** carefully permissioned tools for files, code, local applications, and other workflows.

The UI should remain simple even as the capabilities grow. Advanced runtime settings, tool permissions, diagnostics, and model management belong in secondary panels—not in the main conversation flow.

## Current technology direction

- **Language:** C++20
- **Build:** CMake
- **Desktop UI:** Qt 6 and QML
- **Primary platform:** Windows 10/11 x64
- **Primary accelerator:** NVIDIA CUDA
- **Inference today:** optional llama.cpp for GGUF and ONNX Runtime GenAI for compatible ONNX model folders; CUDA discovery is optional and degrades to a stub
- **Testing:** CTest with small dependency-light C++ tests

Kestrel does not treat CUDA as an inference engine by itself. CUDA supplies device and acceleration facilities; a model runtime is still required. The current inference adapters are llama.cpp (GGUF) and optional ONNX Runtime GenAI (compatible ONNX model folders). TensorRT is **not** an interactive inference backend in this baseline: the separate engine tool validates/records compatibility metadata and does not convert models or run generation.

## Repository layout

```text
.
├── CMakeLists.txt             # Top-level build and feature options
├── README.md                  # Project and contributor documentation
├── src/
│   ├── app/
│   │   ├── main.cpp           # Qt application entry point and --print-runtime
│   │   ├── appcontroller.h
│   │   └── appcontroller.cpp  # QML-facing application state/controller
│   ├── core/
│   │   ├── conversation.h
│   │   ├── conversation.cpp    # Conversation and message domain model
│   │   ├── persona.*           # Tone profile, presence line, anticipatory lines
│   │   ├── presence.*          # What Kestrel and the user last did, for the UI
│   │   ├── idlepersona.*       # The sandboxed loop that runs between turns
│   │   ├── voicesession.h
│   │   └── voicesession.cpp    # Voice response timeline state machine
│   ├── runtime/
│   │   ├── modelbackend.h      # Backend-agnostic model contract
│   │   ├── cudadevice.*        # Portable device/capability types and formatting
│   │   ├── cudadiscovery_cuda.cpp  # Real device discovery (only CUDA-including TU)
│   │   ├── cudadiscovery_stub.cpp  # No-toolkit fallback with the same interface
│   │   ├── engineartifact.*    # Engine build records and compatibility checks
│   │   ├── backendregistry.*   # Backend selection and runtime diagnostics
│   │   ├── mockbackend.*       # Development/demo streaming backend
│   │   ├── llamacppbackend.*   # Optional GGUF inference adapter
│   │   ├── ortgenaibackend.*   # Optional ONNX Runtime GenAI adapter
│   │   └── chatformat.*        # Structured message and fallback template helpers
│   └── ui/
│       └── Main.qml            # Initial soft-glass desktop workspace
└── tests/
    ├── core_tests.cpp          # Core behavior tests
    ├── app_tests.cpp           # Qt threading, send path, and presence tests
    └── runtime_tests.cpp       # Device discovery, engine records, backend selection
```

### Architectural boundaries

The intended dependency direction is:

```text
QML UI → AppController → core domain + ModelBackend
                                  ↓
                    Mock / llama.cpp / ONNX Runtime GenAI
```

- `core` should contain application concepts and business rules, not Qt UI details or vendor-specific runtime calls.
- `runtime` owns model loading, generation, cancellation, and runtime metrics behind `ModelBackend`.
- `app` adapts the core and runtime layers to Qt/QML properties and invokable methods.
- `ui` displays state and sends user intent. QML should not call CUDA, TensorRT, or model APIs directly.
- `tests` should prefer deterministic tests of core logic and backend contracts.

### Vendor headers

`src/runtime/cudadiscovery_cuda.cpp` is the **only** translation unit permitted to
include a CUDA header. The SAPI adapter is similarly isolated in
`src/runtime/sapirecognizer_win32.cpp`. Everything else—including the portable
runtime interfaces, `AppController`, and QML—uses project-owned types. CMake
swaps the CUDA implementation for a portable stub when the toolkit is absent,
so a build without CUDA still compiles, runs, and explains itself.

Keep that boundary intact. Widening vendor headers into portable files makes the
project unbuildable on contributor machines that lack the SDKs, which is the
opposite of what Kestrel wants.

## Prerequisites

### Required for the core and tests

- Windows 10/11 x64, or a current Linux distribution (Ubuntu 24.04 or newer;
  22.04 works once CMake is upgraded, as noted below)
- CMake 3.24 or newer
- A compiler with C++20 support
- Ninja or another supported CMake generator

### Required for the desktop application

- Qt 6.6 or newer
- Qt modules:
  - Core
  - Quick
  - QuickControls2
- A Qt installation matching the compiler and architecture used by CMake, such as MSVC 2022 64-bit on Windows

### Required for CUDA development

- A compatible NVIDIA GPU and current NVIDIA driver
- CUDA Toolkit compatible with the chosen TensorRT release
- CUDA compiler/runtime and development libraries required by the selected backend

### Optional inference dependencies

These are intentionally not vendored in this repository:

- TensorRT SDK, including headers, libraries, runtime DLLs, and `trtexec`
- TensorRT-LLM, if adopted for engine generation or LLM-specific optimizations
- llama.cpp, when built from source rather than consumed as a system package
- Model conversion tooling and model files

Do not commit model files, engine files, SDK binaries, credentials, or machine-specific build output. The repository's `.gitignore` excludes common model and generated-artifact formats.

## Building Kestrel

Run these commands from a shell where the intended compiler, Qt, CMake, Ninja, and CUDA paths are available.

### Configure and build the UI

```powershell
cmake -S . -B build -G Ninja -DKESTREL_BUILD_UI=ON
cmake --build build
```

If CMake cannot find Qt, provide the Qt installation prefix explicitly:

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_PREFIX_PATH="C:/Qt/6.9.0/msvc2022_64" `
  -DKESTREL_BUILD_UI=ON
cmake --build build
```

The exact Qt path depends on the local installation and should not be hard-coded into project source files.

### Build without Qt

The core library and tests can be built independently of Qt:

```powershell
cmake -S . -B build-core -G Ninja -DKESTREL_BUILD_UI=OFF
cmake --build build-core
```

### Run tests

```powershell
ctest --test-dir build-core --output-on-failure
```

For multi-config generators, pass the configuration:

```powershell
ctest --test-dir build --build-config Debug --output-on-failure
```

### Useful CMake options

| Option | Default | Purpose |
| --- | --- | --- |
| `KESTREL_BUILD_UI` | `ON` | Build the Qt/QML application when Qt is available |
| `KESTREL_BUILD_TESTS` | `ON` | Build and register the core and runtime tests |
| `KESTREL_ENABLE_CUDA` | `ON` | Compile real CUDA device discovery. Degrades to the portable stub when no toolkit is found, so it is safe to leave on |
| `KESTREL_ENABLE_LLAMA_CPP` | `ON` | Link llama.cpp for GGUF inference. Degrades to an unavailable backend when not found |
| `KESTREL_LLAMA_CPP_ROOT` | *(empty)* | Path to a llama.cpp install or build tree (must contain `include/llama.h`) |

llama.cpp resolves from its own CMake package first, then from
`KESTREL_LLAMA_CPP_ROOT`. It is never found by accident, because a mismatched
llama.cpp would produce subtly wrong tokens rather than a link error.

### Checking the detected runtime

The desktop binary can report what it actually found without opening a window,
which is the quickest way to confirm the selected model backend and optional
voice integrations:

```powershell
.\build\kestrel.exe --print-runtime
```

It prints the active backend, GPU probe, speech/dictation availability, and
a diagnostics table that explains why an optional integration is unavailable.

### Smoke-testing a real model

`--print-runtime` reports what was found; `--smoke-test` exercises it. It
drives the real window rather than the controller, so it is the only check that
covers the whole path at once: the QML scene loads, a message reaches the
backend on its worker thread, and the streamed tokens land in the model the UI
renders from.

```powershell
.\build\kestrel.exe --smoke-test --model D:\models\qwen.gguf
```

It exits with a meaning, which is the point of the exit codes:

| Exit | Meaning |
| --- | --- |
| `0` | A reply came back from the requested model |
| `1` | The model was asked for and did not load, did not finish loading in time, or did not answer |
| `2` | No model is loaded, so there is nothing to exercise |

The refusal matters as much as the pass. The built-in preview backend answers
instantly and reports itself available, so a check that only asks "is the
runtime available" passes against a canned reply. The run is refused unless a
model path is actually loaded, and a load that was requested has to finish
without an error before the message is sent.

```powershell
.\build\kestrel.exe --smoke-test   # exits 2
```

That refusal is the cheap negative control. It is worth running after changing
anything in the startup path, because it is the difference between a check that
fails when the model is broken and one that passes anyway.

Generated directories such as `build/`, `build-*`, and `cmake-build-*` are ignored by Git. It is safe to delete and recreate them when changing generators or toolchains.

## Windows toolchain notes

Visual Studio provides the MSVC compiler and Windows SDK. Standalone CMake and Ninja may also be installed globally. The compiler still needs to be discoverable by the shell used to configure the project.

If `cl.exe`, `rc.exe`, or `mt.exe` cannot be found, open a Visual Studio Developer Command Prompt/PowerShell or initialize the Visual Studio build environment before invoking CMake. This is not a dependency on Visual Studio's bundled CMake or Ninja; it is required because MSVC and the Windows SDK use environment variables and library paths.

For CUDA, verify the toolkit and driver independently before configuring a backend. A successful CMake build does not prove that a model runtime can load an engine or use the GPU.

## Linux

Linux is a supported build and install target. The portable core, the tests, and
the desktop application all build there, and the runtime reports what it really
found: a Linux box without an NVIDIA driver still gets working CPU inference
through llama.cpp, and one without a CUDA toolkit still builds, because device
discovery degrades to the portable stub.

### Prerequisites

The distribution packages cover the core build; the desktop target needs
versions Ubuntu does not currently ship, so the two are listed separately.

```bash
sudo apt-get install -y build-essential ninja-build git pipx
```

For the core, tests, and the engine-build tool, any CMake 3.24 or newer:

```bash
sudo apt-get install -y cmake       # 3.28 on Ubuntu 24.04
```

Ubuntu 22.04 ships CMake 3.22, which is below the minimum, so upgrade it from
Kitware's own APT repository:

```bash
sudo apt-get install -y ca-certificates gpg wget
wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null \
  | gpg --dearmor - \
  | sudo tee /usr/share/keyrings/kitware-archive-keyring.gpg >/dev/null
echo 'deb [signed-by=/usr/share/keyrings/kitware-archive-keyring.gpg] \
  https://apt.kitware.com/ubuntu/ jammy main' \
  | sudo tee /etc/apt/sources.list.d/kitware.list >/dev/null
sudo apt-get update
sudo apt-get install cmake
```

Substitute `noble` for `jammy` on 24.04. Nothing else in the core build needs
changing.

For the desktop application, Qt 6.6 or newer. No Ubuntu release ships it: 22.04
has 6.2.4 and 24.04 has 6.4.2, so `apt install qt6-base-dev` will not do. Get it
from qt.io or install the same build the CI uses:

```bash
pipx install aqtinstall
aqt install-qt linux desktop 6.9.2 gcc_64 -O "$HOME/Qt"
```

`pipx`, not `pip install --user`: Ubuntu 23.10 and newer mark the system Python
as externally managed, so pip refuses to install into it and aborts with
`error: externally-managed-environment` before aqtinstall is ever downloaded.
`pipx` keeps the tool in its own environment and links `aqt` into
`~/.local/bin`; run `pipx ensurepath` once if that directory is not already on
your `PATH`.

That provides Qt Core, Qt Quick, and Qt Quick Controls 2. Without a Qt 6.6 or
newer prefix, CMake skips the desktop target and builds the core and its tests,
which is the same degradation a contributor without Qt gets on any platform.

### Build, test, and install

```bash
cmake -S . -B build -G Ninja \
  -DKESTREL_BUILD_UI=ON \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.9.2/gcc_64"
cmake --build build
ctest --test-dir build --output-on-failure
sudo cmake --install build
```

The install places the application where a freedesktop session looks for it:

| Installed path | Contents |
| --- | --- |
| `bin/kestrel` | the desktop application |
| `share/applications/io.github.canadiancowboy.kestrel.desktop` | the launcher entry |
| `share/icons/hicolor/scalable/apps/` | the application icon |
| `share/metainfo/` | AppStream metadata, so the app appears in software centres |

To try an install without touching the system, stage one into a prefix you
control:

```bash
cmake --install build --prefix "$HOME/.local"
```

`kestrel-engine-build` is deliberately not installed. It writes engine records
next to an engine and is a maintainer tool, so it stays in the build tree rather
than on an end user's `PATH`.

### Linking llama.cpp

llama.cpp is the portable inference path and the one most likely to be present
on a Linux workstation. Build it as shared libraries and install it to a prefix:

```bash
git clone https://github.com/ggml-org/llama.cpp
cmake -S llama.cpp -B llama.cpp/build -G Ninja -DBUILD_SHARED_LIBS=ON
cmake --build llama.cpp/build
cmake --install llama.cpp/build --prefix "$HOME/.local"
```

Then configure Kestrel against that prefix:

```bash
cmake -S . -B build -G Ninja \
  -DKESTREL_BUILD_UI=ON \
  -DKESTREL_LLAMA_CPP_ROOT="$HOME/.local"
```

llama.cpp splits its shared objects across `libllama.so`, `libggml.so`,
`libggml-base.so`, `libggml-cpu.so`, and one more per enabled backend, so CMake
links whichever of those the prefix actually contains and records the prefix in
the binary's rpath. Without that rpath the installed binary would start and then
fail to load `libllama.so`, because the Linux loader searches the system
directories only.

When llama.cpp is not found, the GGUF backend reports itself unavailable and the
mock backend stays usable. That is the documented degradation, not a failure.

### Display servers

Both X11 and Wayland work. Two platform details are deliberate: `main.cpp` pins
the Basic Quick Controls style everywhere, because the hand-drawn QML is
discarded by a native style, and it announces Kestrel's desktop file id so the
taskbar shows the application's name and icon instead of the bare executable
name.

## Development workflow

1. Create or update a focused branch for the change.
2. Read the surrounding code and preserve the existing C++/Qt style.
3. Keep changes small and maintain the layer boundaries.
4. Build the core and run tests frequently.
5. Build the UI when touching Qt/QML code.
6. Test failure paths, cancellation, and unavailable optional dependencies—not only the happy path.
7. Review `git diff` and `git diff --check` before submitting a change.

A typical local verification sequence is:

```powershell
cmake -S . -B build-core -G Ninja -DKESTREL_BUILD_UI=OFF -DKESTREL_BUILD_TESTS=ON
cmake --build build-core
ctest --test-dir build-core --output-on-failure
git diff --check
```

When Qt is installed:

```powershell
cmake -S . -B build -G Ninja `
  -DCMAKE_PREFIX_PATH="C:/path/to/Qt/6.x.x/msvc2022_64" `
  -DKESTREL_BUILD_UI=ON
cmake --build build
```

## Coding guidelines

### C++

- Use C++20 features only when they improve clarity or correctness.
- Prefer clear ownership and RAII over raw owning pointers.
- Keep public interfaces narrow and use `const`/`noexcept` where appropriate.
- Avoid blocking the UI thread during model loading or generation.
- Make cancellation a first-class behavior.
- Return actionable errors rather than swallowing failures.
- Keep vendor headers and implementation details inside runtime adapters.

### Qt and QML

- Keep QML focused on presentation and user interaction.
- Expose stable, small view-model/controller APIs from C++.
- Use signals for state changes and avoid unnecessary polling.
- Preserve keyboard accessibility and sensible minimum window sizes.
- Keep animation subtle and avoid visual noise.

### CUDA and TensorRT

- Treat GPU resources as explicitly owned and released.
- Check CUDA/TensorRT return codes and include useful context in errors.
- Never assume a model engine is portable across GPU architectures, TensorRT versions, or CUDA versions.
- Keep engine building/conversion separate from the interactive desktop process where possible.
- Avoid adding CUDA-X libraries merely because they exist; each dependency should have a demonstrated runtime or model-conversion need.

## Native voice conversation

Voice is a first-class part of Kestrel's conversation experience, not a separate assistant mode. The goal is a native, local voice loop with effectively immediate interaction: speech should begin quickly, the user should be able to interrupt naturally, and Kestrel should never force the user to wait for an answer to finish before accepting new information.

### Setting up a local voice

Kestrel speaks through Qt's text-to-speech when it is available, but the voices a stock Windows install offers were recorded before neural speech existed, and no better one can be added as a *system* voice because none of the good engines are SAPI. So Kestrel can run its own local model instead, and prefers it when it finds one.

The model is not vendored: it is a per-machine download, a few hundred megabytes, and something a contributor chooses for their own hardware. When it is absent, Kestrel falls back to the platform voice and says so rather than pretending to have none.

From the repository root:

```bash
# 1. An interpreter with the model bindings. A venv keeps it off the system.
py -m venv .kestrel-voice
./.kestrel-voice/Scripts/python.exe -m pip install kokoro-onnx soundfile numpy

# 2. The model (325 MB) and its voice table (28 MB).
mkdir -p .kestrel-voice/models
curl -L -o .kestrel-voice/models/kokoro-v1.0.onnx \
  https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/kokoro-v1.0.onnx
curl -L -o .kestrel-voice/models/voices-v1.0.bin \
  https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.0/voices-v1.0.bin
```

That is the development setup for the optional Kokoro voice driver. On Windows, Kestrel searches beside the executable and its parent for `.kestrel-voice/Scripts/python.exe`, the model, and the driver script. Runtime status may say that the voice is loading; it does not guarantee that a cold model load will succeed.

```bash
build/kestrel.exe --print-runtime
#   voice        : unavailable (the Kokoro voice is still loading)
#   dictation    : unavailable (preview recognizer (no microphone))
```

The exact report depends on build options and local files. The driver process starts asynchronously, so the initial status can say that Kokoro is still loading. The app holds an answer briefly for an installed voice; if the engine fails or exceeds the bounded startup window, the answer remains available as text. `--print-runtime` reports once at startup and does not wait for the model to become ready.

Speech playback is incremental by clause, not full-duplex. `VOICE PROFILE` switches among voices exposed by the selected local engine; an unknown name is refused rather than deferred to a later synthesis failure.

`.kestrel-voice/` is git-ignored. Override the location with `KESTREL_VOICE_PYTHON`, the model directory with `KESTREL_VOICE_MODEL_DIR`, and the default voice with `KESTREL_VOICE`.

Kokoro is the currently wired local neural TTS engine. The driver is an optional subprocess, and model/dependency setup and licensing obligations are described in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Piper is not wired into the current baseline.

### Speech input

Dictation uses SAPI 5, the desktop engine that has shipped with Windows since
2000: `CLSID_SpSharedRecognizer`, an `ISpRecoContext`, and the default capture
device. It is an ordinary COM class, so it activates with `CoCreateInstance`, and
every call is a vtable call through an interface declared in the Windows SDK's
`sapi.h` — no Speech SDK install, and no `sapi.lib`. The only library it links
is `winmm`, for the device count in the microphone probe.

The Windows Runtime path is not an alternative. `Windows.Media.SpeechRecognition`
exposes the `ISpeechRecognizer` ABI interface but no projected runtime class, so
there is no activation factory to create and no `GetSpeechRecognizerAsync` to
call, and its desktop flavour has no interim-text event either.

Which recognizer Kestrel uses is decided at startup from a cheap capture-device
count, so nothing has to be opened to ask the question:

| Machine has | Recognizer | Reports itself as |
| --- | --- | --- |
| a capture device | SAPI 5, on a worker thread of its own | `Windows SAPI 5 recognizer, N capture device(s), finished phrases only` |
| no capture device, or no usable SAPI adapter | the scripted preview recognizer | `preview recognizer (no microphone)` |

`KESTREL_SPEECH_INPUT` forces either branch: `auto` (the default) probes,
`mock` always uses the scripted preview recognizer, and `platform` asks for SAPI
and reports what the engine says rather than what a device count guessed. The
test suite pins itself to `mock`, but the preview recognizer is deliberately
reported as **not** a real microphone.

SAPI reports completed phrases, not streaming partial words. Current playback
interrupts at a clause boundary, not at an arbitrary word, and the pipeline has
no VAD or acoustic echo cancellation. It is therefore an adapter baseline, not
a claim of natural low-latency full-duplex voice. See
[src/runtime/sapirecognizer.h](src/runtime/sapirecognizer.h) for the current STT contract.

### Voice goals

- **Modular and customizable:** capture, VAD, STT, turn management, inference, TTS, playback, and acoustic echo cancellation should remain replaceable components rather than one hard-coded runtime choice.
- **Local-first audio:** microphone capture, recognition, response generation, and synthesis should run locally when suitable models and hardware are available.
- **Low perceived latency:** recognize and respond incrementally without UI-blocking work or unnecessary full-response waits.
- **Natural turn-taking:** eventually support interruption finer than the current clause boundary, with echo-aware full-duplex behavior where the selected components permit it.

The present app does not yet implement this advanced target: SAPI provides completed phrases, playback uses clause segments, and there is no VAD/AEC stage. Keep runtime choices open until target hardware, languages, licensing, and measured latency are known. See the [proposed extensible voice runtime design](docs/VOICE_RUNTIME_DESIGN.md) for the architecture and phased roadmap.
- **Transparent state:** listening, thinking, speaking, paused, interrupted, and error states should be visible without making the interface feel busy.

“Zero latency” is an interaction goal rather than a physically literal guarantee. Contributors should optimize measured time-to-first-audio, time-to-first-token, and interruption response time, and document hardware and model conditions when reporting results.

### Interruption and barge-in behavior

The desired advanced behavior is to detect a user interruption during playback and stop or pause at the earliest safe audio boundary. The current implementation only accepts a request through the listening path and stops playback at a clause boundary; full-duplex echo-aware detection is not implemented. The future interruption path should:

1. Detect the user's speech while synthesized audio is playing.
2. Stop or pause playback at the earliest safe audio boundary.
3. Preserve the exact assistant output that has already been spoken.
4. Capture and transcribe the user's interruption.
5. Decide whether to resume, revise, or abandon the unfinished response.
6. Continue the conversation using the new information.

Interruption must also cancel or reprioritize work in the generation pipeline where appropriate. Stopping audio alone is not sufficient if the model continues consuming GPU time and produces stale output that later reaches the user.

### Pause, resume, and unfinished responses

Kestrel should maintain an explicit response playback state rather than treating speech as one uninterrupted string. A response may be:

- `queued`
- `generating`
- `speaking`
- `paused`
- `interrupted`
- `completed`
- `cancelled`
- `failed`

When speech is interrupted, Kestrel should remember at least:

- The complete assistant response generated so far.
- The portion already delivered as audio.
- The text/audio segment where playback stopped.
- Any user words captured during the interruption.
- The conversation context and runtime settings used for the response.

This lets the user ask Kestrel to continue later. “Continue” should resume from the correct semantic point rather than repeating the entire answer. If the new information changes the answer, Kestrel should be able to reword or replace the remaining response instead of blindly continuing stale text.

### Response revision rules

A new user interruption can have different meanings. The conversation layer should distinguish between:

- **Pause:** temporarily stop speaking and preserve the current response for later continuation.
- **Correction:** revise the unfinished answer using newly supplied facts.
- **Question:** answer the interruption first, then optionally return to the previous response.
- **Replacement:** discard the unfinished response because the user's goal changed.
- **Resume:** continue the preserved answer from its semantic cutoff.

These decisions should be represented as structured conversation events, not inferred only from UI text. The UI should make the active behavior understandable and provide controls to resume, regenerate, revise, or discard an interrupted answer.

### Audio and runtime architecture

The intended voice pipeline is:

```text
Microphone
    ↓
Native audio capture + echo cancellation
    ↓
Voice activity detection / interruption detector
    ↓
Local speech recognition
    ↓
Conversation state and model backend
    ↓
Streaming text tokens
    ↓
Incremental text-to-speech
    ↓
Native audio playback
```

The pipeline must support cancellation and backpressure at every stage. Audio playback should consume incremental speech segments while the language model is still generating; it should not wait for the full response. The conversation state must remain authoritative so text generation, speech synthesis, and playback cannot disagree about which response is current.

Voice-specific implementation concerns include:

- Full-duplex microphone capture and speaker playback.
- Echo cancellation so Kestrel does not transcribe its own voice.
- Voice activity detection with configurable sensitivity.
- Partial speech-recognition results for faster turn-taking.
- Sentence or clause-level TTS chunking rather than full-response synthesis.
- Immediate playback cancellation with no stale audio queued afterward.
- Synchronization between spoken text offsets and generated text offsets.
- A fallback text-only mode when audio devices or voice models are unavailable.
- Privacy controls for microphone access and local audio retention.

Voice work should be implemented behind application-level interfaces so the core conversation model can be tested without physical audio hardware. Deterministic tests should cover interruption, pause/resume, cutoff tracking, response revision, stale-output cancellation, and recovery from audio or recognition failures.

## Runtime and model strategy

The backend contract in `src/runtime/modelbackend.h` is the seam between the application and inference implementation. A backend is responsible for:

- Reporting availability and runtime status
- Loading and unloading a model or TensorRT engine
- Streaming generated tokens
- Cancelling active generation
- Returning useful errors
- Reporting basic generation/context metrics

### The shared system prompt

A system prompt is identical on every turn and is often longer than the reply
it precedes, so reprocessing it per request is the largest avoidable cost in a
chat turn. `ModelBackend::setSystemPrompt()` declares it once; a backend that
can hold a cache decodes it into the context one time and leaves its KV entries
resident, and each turn then pays only for its own tokens.

The contract has one rule callers must respect: **pass `generate()` the
per-turn prompt only.** The prefix is the backend's business, and repeating it
in the request undoes the caching as well as double-counting the context.

The reconciliation happens on the worker thread, not in the setter, because a
model context is not safe to touch from the UI thread. When the prefix is
unchanged only the tokens after it are dropped, so it survives; when it changes
the cache is rebuilt once. The diagnostics panel's SHARED PREFIX row shows how
many tokens are actually resident, so the saving is visible rather than
theoretical. Backends that cannot cache it report the prefix's cost on every
turn instead, which is what the mock does.

### KV cache accounting

Token counts alone hide the dominant cost. A context looks the same size whether
it holds 4k or 128k positions, but the KV cache behind it grows linearly with
context, and it overtakes the weights as the term that matters.

llama.cpp exposes no accessor for the resolved KV allocation, so the total is
computed from the model's own shape — layer count, head count, grouped-query
head count, the context's cell count, and the KV element types the context was
created with (via `ggml_row_size`, so a quantized cache is measured rather than
assumed to be f16). A model whose shape does not divide cleanly reports zero
rather than a guess. For a Qwen2.5-0.5B model at 4096 cells this computes to
48.0 MiB, matching the 48.00 MiB llama.cpp itself reports.

The mock backend exists so the UI can be developed and reviewed without CUDA, TensorRT, or a model. It should remain usable in development and automated tests.

The TensorRT direction requires an additional model preparation workflow. A typical future flow will be:

```text
Source model/checkpoint
        ↓
Conversion and engine build tooling
        ↓
TensorRT engine/plan for the target GPU/runtime
        ↓
Kestrel TensorRT C++ backend
        ↓
CUDA execution on the local GPU
```

Engine files are machine- and version-sensitive and should remain local artifacts, not repository assets.

### Engine build records

A serialized TensorRT engine is bound to the TensorRT version that produced it,
to a CUDA version, and to the GPU architecture it was built for. Deserializing a
mismatched engine fails deep inside TensorRT with a message that does not tell
the user what to do.

To make that failure predictable, the offline engine-build step writes a small
sidecar record next to each engine:

```text
model.plan
model.plan.kestrel-engine
```

The record is plain `key=value` text so it stays diffable and inspectable, and it
introduces no serialization dependency:

```text
# Kestrel engine build record. Written by the offline engine-build step.
tensorrt_version=10400
cuda_version=13040
compute_major=8
compute_minor=9
gpu_name=NVIDIA GeForce RTX 4060
built_by=trtexec 10.4
```

`TensorRTBackend::loadModel()` reads it and compares it against the live CUDA
probe **before** any deserialization is attempted, refusing the artifact with an
explanation when the architecture, driver, or TensorRT version cannot work. A
missing record is treated as unknown rather than invalid: absence of metadata is
not evidence of a problem, so it warns instead of blocking.

`writeEngineBuildRecord()` and `readEngineBuildRecord()` in
`src/runtime/engineartifact.h` are the API. The writer is intended for the
offline tooling; the desktop process only ever reads records and must never
modify an engine.

### Building an engine

`kestrel-engine-build` does the conversion and writes the record. It is a
separate executable rather than an app mode, because building an engine is a
long, machine-specific operation and the desktop process must never write to
engines.

```bash
# Convert a model and write its build record.
kestrel-engine-build --model model.onnx --output model.plan

# Annotate an engine that was built elsewhere, without converting anything.
kestrel-engine-build --engine model.plan --record-only
```

The record is always written from the GPU and CUDA actually present on the
machine running the tool, never from values passed on the command line. A
record that does not describe reality is worse than no record, because
validation would then wave a mismatched engine through. After writing, the tool
reads the record back and runs the same compatibility check the app uses, so
the validation path is exercised end to end even without a real engine
present.

Conversion requires a TensorRT SDK. Without one the tool still writes records
for `--record-only` and explains how to enable conversion.

## Generation and voice architecture

Generation never runs on the UI thread. `AppController` hands the request to
`GenerationWorker`, which lives on its own `QThread`; tokens and completion come
back as queued signals and are applied to QML-visible state on the UI thread.

Cancellation is the one deliberate exception to "everything is queued". It is
called directly from the UI thread and only performs atomic stores, because the
worker's event loop is blocked inside `generate()` and would not service a
queued call until generation had already finished, which is exactly too late.
`ModelBackend` documents this contract: a backend must poll its cancellation
flag between tokens, and must make `cancel()` safe to call from another thread.

Voice pacing is decided in `VoiceSession` rather than in the interface, because
the same decisions apply whether audio is synthesized locally or handed to a
platform voice later. `planSpeech` splits generated text into clause-sized
segments with the micro-pause that belongs between them, and `nextSpeechSegment`
returns the next unspoken clause with absolute offsets into the response — which
is what lets speech begin before generation finishes. A `VoicePersona` (voice id,
rate, pitch, warmth, and three pause lengths) makes the pacing a value rather than
a hardcoded constant.

`VoiceSession` drives the visible timeline, and audio is delivered clause by
clause as each one finishes rather than in one lump when generation ends. Where
there is no engine -- a build without Qt TextToSpeech, a machine with no voice
installed, or a local engine that is still loading when the reply is finished --
the same timeline runs with playback treated as delivered immediately, which is
the text-only fallback the state machine defines for exactly that situation. The
two paths are the same code with a different backend behind it, not two
behaviours.

Pause stops delivery while preserving the response for `resume()`,
which reopens generation for whatever text was still owed; sending a new
message mid-response is a barge-in, which abandons the interrupted response
and hands the timeline to the new prompt. The pause/resume control is only
shown when the state machine says the transition is legal, so it can never be a
button that silently does nothing.

## Assistant presence and idle autonomy

Kestrel is a presence, not a request box. Three portable pieces in `src/core/`
carry that, and none of them is allowed to reach outside the process.

**`Persona`** holds the tone profile and five dials (`focus`, `curiosity`,
`initiative`, `calmness`, `presenceIntensity`). It produces the assistant-presence
line that goes into the shared system prompt, the short acknowledgement said the
moment a request is accepted, the one-line status whisper, and the anticipatory
lines ("Would you like me to continue?", "Task complete."). The presence line is
built from the fixed tone profile rather than the drifting dials, so it is
byte-identical on every turn and the backend's cached prefix survives; the dials
move, the rules do not.

**`Presence`** records what the user did, what the assistant did, the voice and
generation states, and the mood as plain booleans. The UI animates on that
snapshot instead of on raw events, so a pulse means the same thing whether it
came from a keystroke, a barge-in, or the idle loop. Time is injected by the
owner rather than read from a clock, which keeps the easing deterministic.

**`IdlePersona`** is the loop that runs when nobody is talking. It stays completely
silent while a turn is generating, the voice is live, or something is typed and
unsent; it drifts the dials over time and weights its own work by them, so
personality is arithmetic rather than prompt text. Because the dials also decay
every cycle, the loop is a cycle and not a ramp: an assistant that only ever
gained curiosity and initiative would eventually become someone nobody wants to
talk to.

The safety boundary is stated as data in `IdlePolicy`, and it is deliberately
narrower than it looks:

- The task set is closed, and every entry is a string, a number, or an enum.
  There is no "run a command" or "call an API" member, because a loop that can be
  handed arbitrary work stops being a screensaver and starts being an unattended
  agent.
- There is no network or filesystem permission to grant, because the loop has no
  way to reach either.
- `ModelWarmup` is the only task that leaves pure computation, and it is off
  until the user turns it on. Its tokens are discarded; the point is warm caches.
- Idle thoughts are internal. They are populated always and displayed only when
  the user asks to see them.

Contributors adding to this layer must keep it that way. Anything that needs the
network, the filesystem, or a destructive action is an agent tool, and it belongs
behind `ModelBackend`-style permission, in the timeline, with confirmation — not
in the idle loop.

## Safety and privacy principles

Kestrel is intended to be local-first, but contributors must not assume that “local” automatically means safe. Future tools must:

- Be disabled by default.
- Declare their permissions and affected resources.
- Show meaningful activity in the conversation timeline.
- Require confirmation for destructive or external actions.
- Support cancellation and failure reporting.
- Avoid silently uploading files, prompts, telemetry, or credentials.

Never add real secrets, API keys, private model files, user data, or system-specific paths to source control.

## Roadmap

### Foundation

- [x] C++20/CMake project scaffold
- [x] Core conversation model
- [x] Backend abstraction
- [x] Mock streaming backend
- [x] Initial Qt/QML shell
- [x] Dependency-light core tests

### Runtime

- [x] Add CUDA device discovery and capability reporting
- [x] Add optional TensorRT SDK discovery and engine-artifact compatibility records
- [x] Validate recorded engine metadata against the live device
- [ ] Implement TensorRT model conversion and interactive inference (not currently available)
- [x] Implement asynchronous token generation and cancellation
- [x] Expose GPU memory and throughput metrics to the UI
- [x] Document the intended model preparation workflow
- [x] Provide the offline engine metadata/compatibility tool (does not convert models)
- [x] Link llama.cpp for GGUF inference behind an auto-degrading option
- [x] Make context accounting backend-driven so a loaded model reports exact counts
- [x] Run the full build and test matrix in CI on Windows, Linux, and macOS
- [x] Cache the shared system prompt as a reusable prefix instead of resending it
- [x] Report KV cache occupancy in bytes alongside the token count
- [x] Load a GGUF from disk through a model picker instead of only the mock

### Application

- [x] Persist conversations and profile settings locally
- [x] Select and load compatible model backends (GGUF and ONNX Runtime GenAI)
- [ ] Add advanced inference engine configuration and benchmarking
- [ ] Improve markdown and code rendering
- [x] Add search, rename, delete, and conversation management
- [x] Add bounded model loading, errors, preview fallback, and recovery states

### Agent capabilities

- [x] Define a permissioned idle-tool interface
- [x] Record permissioned idle-tool results in the conversation transcript
- [ ] Add filesystem/code tools behind explicit user approval
- [ ] Add configurable sandboxing and policy controls

## Contributing

Contributions are welcome, especially in the areas of runtime adapters, advanced local voice, Qt/QML interaction design, testing, documentation, and safety-oriented tool boundaries.

Before opening a change:

- Explain the user problem or engineering motivation.
- Describe the chosen approach and important tradeoffs.
- Include tests for behavior changes where practical.
- Include build/configuration notes when adding a dependency.
- State which optional dependencies were available during validation.
- Keep unrelated formatting or generated files out of the change.

For runtime changes, include the relevant GPU, driver, CUDA, runtime, compiler, and model versions in the development notes. TensorRT engine tooling is metadata-only in the current baseline. Do not include personal paths or sensitive data.

### Pull request workflow

Open changes as pull requests against `main`; `.github/PULL_REQUEST_TEMPLATE.md` carries the validation checklist above.

CodeRabbit reviews every pull request automatically. `.coderabbit.yaml` encodes the architectural boundaries that a generic reviewer cannot infer, most importantly that `src/runtime/cudadiscovery_cuda.cpp` is the only translation unit allowed to include a CUDA header and that no change may block the UI thread. If CodeRabbit flags something that is wrong for a stated reason, say so in the thread rather than silently ignoring it.

## License

MIT, with an express patent grant modeled on Apache-2.0 Section 3. The MIT terms govern in full; the patent grant is additive, so you keep MIT's permissiveness while every contributor grants you patent rights with the usual termination-on-litigation clause.

Note that this is a custom variant, not a license GitHub's license picker will recognize automatically, so downstream compliance tooling may not detect it. Apache-2.0 is the battle-tested license that already combines permissive terms with a patent grant; switch to it if recognition by automated tooling matters more than MIT's exact wording.
