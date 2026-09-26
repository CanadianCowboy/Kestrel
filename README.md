# Kestrel

Kestrel is a local-first personal AI desktop agent for Windows systems with NVIDIA GPUs. It is designed to provide a private, focused workspace for chatting with local models and eventually performing explicitly authorized agent tasks such as working with files, code, and developer tools.

The project prioritizes:

- **Local execution and privacy:** prompts, responses, and future agent data should remain on the user's machine unless the user deliberately enables an external integration.
- **A polished, quiet interface:** Kestrel should feel like a streamlined command center rather than a crowded AI dashboard.
- **NVIDIA-first acceleration:** CUDA and NVIDIA's CUDA-X ecosystem are the primary optimization target.
- **A maintainable C++ foundation:** application state, runtime integration, and UI are separated so contributors can work independently.
- **Safe extensibility:** tools and automation must be explicit, inspectable, cancellable, and disabled by default until configured.

> **Project status:** Kestrel is an early scaffold. The C++ core, mock streaming backend, backend contracts, tests, and initial Qt/QML shell exist. CUDA device discovery and engine-artifact validation are in place and reported to the UI. Native model execution, persistent conversations, live throughput telemetry, and agent tools are still under development.

## Vision

Kestrel aims to become a personal local AI environment with three layers:

1. **Conversation:** a fast, private chat interface with streaming output, history, model controls, and useful context handling.
2. **Intelligence runtime:** a native inference layer that uses CUDA and TensorRT on supported NVIDIA hardware.
3. **Agent capabilities:** carefully permissioned tools for files, code, local applications, and other workflows.

The UI should remain simple even as the capabilities grow. Advanced runtime settings, tool permissions, diagnostics, and model management belong in secondary panels—not in the main conversation flow.

## Current technology direction

- **Language:** C++20
- **Build:** CMake
- **Desktop UI:** Qt 6 and QML
- **Primary platform:** Windows 10/11 x64
- **Primary accelerator:** NVIDIA CUDA
- **Target inference direction:** native TensorRT C++ runtime, with model conversion/build tooling kept separate from the desktop application
- **Testing:** CTest with small dependency-light C++ tests

Kestrel intentionally does not treat CUDA as an inference engine by itself. CUDA supplies the GPU programming and math ecosystem; a model runtime is still required. The current direction is to use TensorRT for optimized execution and CUDA libraries such as the CUDA runtime, cuBLAS/cuBLASLt, and NVRTC where appropriate. TensorRT-LLM may be used later for model-specific optimization or engine generation, but it is not required by the current scaffold.

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
│   │   ├── llamacppbackend.*   # Placeholder legacy/experimental adapter boundary
│   │   └── tensorrtbackend.*   # TensorRT adapter boundary
│   └── ui/
│       └── Main.qml            # Initial soft-glass desktop workspace
└── tests/
    ├── core_tests.cpp          # Core behavior tests
    └── runtime_tests.cpp       # Device discovery, engine records, backend selection
```

### Architectural boundaries

The intended dependency direction is:

```text
QML UI → AppController → core domain + ModelBackend
                                  ↓
                    Mock / TensorRT / future backends
```

- `core` should contain application concepts and business rules, not Qt UI details or vendor-specific CUDA calls.
- `runtime` owns model loading, generation, cancellation, and runtime metrics behind `ModelBackend`.
- `app` adapts the core and runtime layers to Qt/QML properties and invokable methods.
- `ui` displays state and sends user intent. QML should not call CUDA, TensorRT, or model APIs directly.
- `tests` should prefer deterministic tests of core logic and backend contracts.

### Vendor headers

`src/runtime/cudadiscovery_cuda.cpp` is the **only** translation unit permitted to
include a CUDA header, and `src/runtime/tensorrtbackend.cpp` is the only one
permitted to include TensorRT headers. Everything else—including
`src/runtime/cudadevice.h`, `AppController`, and QML—sees device facts through
portable structs. CMake swaps `cudadiscovery_cuda.cpp` for
`cudadiscovery_stub.cpp` when no CUDA Toolkit is found, so a build without CUDA
still compiles, runs, and explains itself.

Keep that boundary intact. Widening vendor headers into portable files makes the
project unbuildable on contributor machines that lack the SDKs, which is the
opposite of what Kestrel wants.

## Prerequisites

### Required for the core and tests

- Windows 10/11 x64 or another CMake-supported development platform
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
| `KESTREL_ENABLE_TENSORRT` | `OFF` | Link the TensorRT SDK. Opt-in because the SDK is not vendored |
| `KESTREL_TENSORRT_ROOT` | *(empty)* | Path to an unpacked TensorRT SDK (must contain `include/NvInfer.h`) |

### Checking the detected runtime

The desktop binary can report what it actually found without opening a window,
which is the quickest way to confirm a build picked up the GPU you expect:

```powershell
.\build\kestrel.exe --print-runtime
```

It prints the active backend, the probed device, and a diagnostics table that
also explains why an unavailable backend is unavailable.

Generated directories such as `build/`, `build-*`, and `cmake-build-*` are ignored by Git. It is safe to delete and recreate them when changing generators or toolchains.

## Windows toolchain notes

Visual Studio provides the MSVC compiler and Windows SDK. Standalone CMake and Ninja may also be installed globally. The compiler still needs to be discoverable by the shell used to configure the project.

If `cl.exe`, `rc.exe`, or `mt.exe` cannot be found, open a Visual Studio Developer Command Prompt/PowerShell or initialize the Visual Studio build environment before invoking CMake. This is not a dependency on Visual Studio's bundled CMake or Ninja; it is required because MSVC and the Windows SDK use environment variables and library paths.

For CUDA, verify the toolkit and driver independently before configuring a backend. A successful CMake build does not prove that a model runtime can load an engine or use the GPU.

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

### Voice goals

- **Local-first audio path:** microphone capture, voice activity detection, speech recognition, response generation, and speech synthesis should run locally whenever the required models and hardware support it.
- **Near-zero perceived latency:** the system should begin recognizing speech and responding as early as possible, while avoiding unnecessary buffering, blocking calls, or full-response waits.
- **Native desktop integration:** audio capture and playback should use a native C++ pipeline appropriate for the target platform rather than routing conversation through a browser or remote service.
- **Natural turn-taking:** users should be able to speak while Kestrel is responding, just as they would interrupt a person.
- **Transparent state:** listening, thinking, speaking, paused, interrupted, and error states should be visible without making the interface feel busy.

“Zero latency” is an interaction goal rather than a physically literal guarantee. Contributors should optimize measured time-to-first-audio, time-to-first-token, and interruption response time, and document hardware and model conditions when reporting results.

### Interruption and barge-in behavior

When the user begins speaking while Kestrel is speaking, Kestrel must stop or pause audio playback immediately instead of finishing the current sentence. The interruption path should:

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
- [x] Integrate TensorRT headers and libraries through CMake options
- [x] Validate engine artifacts against the live device before deserializing
- [ ] Implement asynchronous token generation and cancellation
- [ ] Expose GPU memory and throughput metrics to the UI
- [x] Define and document the model conversion workflow

### Application

- [ ] Persist conversations and settings locally
- [ ] Add model/engine selection and configuration
- [ ] Improve markdown and code rendering
- [ ] Add search, rename, delete, and conversation management
- [ ] Add robust loading, error, and recovery states

### Agent capabilities

- [ ] Define a permissioned tool interface
- [ ] Add inspectable tool activity events
- [ ] Add filesystem/code tools behind explicit user approval
- [ ] Add configurable sandboxing and policy controls

## Contributing

Contributions are welcome, especially in the areas of CUDA/TensorRT integration, Qt/QML interaction design, testing, documentation, and safety-oriented tool boundaries.

Before opening a change:

- Explain the user problem or engineering motivation.
- Describe the chosen approach and important tradeoffs.
- Include tests for behavior changes where practical.
- Include build/configuration notes when adding a dependency.
- State which optional dependencies were available during validation.
- Keep unrelated formatting or generated files out of the change.

For runtime changes, include the relevant GPU, driver, CUDA, TensorRT, compiler, and model/engine versions in the development notes. Do not include personal paths or sensitive data.

### Pull request workflow

Open changes as pull requests against `main`; `.github/PULL_REQUEST_TEMPLATE.md` carries the validation checklist above.

CodeRabbit reviews every pull request automatically. `.coderabbit.yaml` encodes the architectural boundaries that a generic reviewer cannot infer, most importantly that `src/runtime/cudadiscovery_cuda.cpp` is the only translation unit allowed to include a CUDA header and that no change may block the UI thread. If CodeRabbit flags something that is wrong for a stated reason, say so in the thread rather than silently ignoring it.

## License

MIT, with an express patent grant modeled on Apache-2.0 Section 3. The MIT terms govern in full; the patent grant is additive, so you keep MIT's permissiveness while every contributor grants you patent rights with the usual termination-on-litigation clause.

Note that this is a custom variant, not a license GitHub's license picker will recognize automatically, so downstream compliance tooling may not detect it. Apache-2.0 is the battle-tested license that already combines permissive terms with a patent grant; switch to it if recognition by automated tooling matters more than MIT's exact wording.
