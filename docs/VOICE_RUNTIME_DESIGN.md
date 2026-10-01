# Extensible voice runtime design

**Status:** proposed architecture and roadmap; not a claim about current shipped capability.

Kestrel should grow from its current adapter seams into a low-latency, full-duplex voice system whose capture, speech recognition, voice activity detection, acoustic echo cancellation, and speech synthesis can be selected and replaced independently. The design goal is not to bind the product to one model, OS API, audio library, or vendor. A voice session should be able to run entirely on-device, report exactly which capabilities are active, and fail over without leaving the microphone, playback device, or assistant turn in an ambiguous state.

This document describes a target. The current implementation is materially simpler: Windows SAPI recognizes completed phrases (not streaming interim words); the current TTS choices are Qt/platform speech or an optional Kokoro path; barge-in generally waits for a clause boundary; there is no production VAD/AEC graph or verified full-duplex path. Keep that distinction visible in the UI and release notes.

## Goals and non-goals

### Goals

- Independently replace audio capture, preprocessing, VAD, streaming STT, AEC, TTS, playback, and orchestration implementations.
- Support partial and final transcripts, interruption, and simultaneous capture/playback with accurate timestamps and explicit cancellation.
- Make local-only operation the default. Network-backed speech is a separately disclosed, opt-in provider, never a silent fallback.
- Keep device and engine failures recoverable: capability discovery, structured diagnostics, bounded queues, restartable sessions, and deterministic cleanup.
- Make voice behavior testable without a microphone, speakers, model weights, or wall-clock sleeps.
- Keep UI and application policy independent from platform and model SDKs.

### Non-goals for the first implementation phases

- Promise identical recognition quality or latency across languages, hardware, and providers.
- Require every backend to support every feature. Providers declare capabilities and the orchestrator negotiates a compatible graph.
- Mix arbitrary binary plugins into the application process without a versioned compatibility, ownership, and trust policy.
- Record or retain raw microphone audio by default.

## Proposed layers

```text
UI / user consent
        │
VoiceSessionController  ── turn policy, interruption, permissions, session IDs
        │
VoicePipeline / graph  ── capabilities, routing, clocks, queue budgets, recovery
        ├── CaptureSource ── device selection, PCM frames, hot-plug
        ├── AudioProcessor(s) ── resample, channel map, denoise / gain (optional)
        ├── AEC processor ◄── PlaybackReferenceTap (rendered assistant audio)
        ├── VAD ── speech onset/offset, endpointing hints
        ├── Streaming STT ── partial, stable, final, confidence, word timing
        ├── Turn broker ── endpointing, transcript edits, user interruption
        ├── LLM token stream ── cancellation tied to this voice turn
        ├── Speech planner ── incremental text chunks and prosody metadata
        ├── Streaming TTS ── timestamped PCM chunks, voice/style selection
        └── PlaybackSink ── output device, playback position, echo reference
```

Each node has a narrow contract and capability descriptor. `VoicePipeline` owns connections and lifecycle; concrete adapters do not call the UI, start an unrelated worker, or reach into `AppController`. A thin Qt-facing controller translates typed events to Qt signals and QML properties. The core pipeline should remain testable as portable C++.

## Runtime contracts

### Shared media and event types

Use owned, immutable values across asynchronous boundaries. An `AudioFrame` should carry PCM samples, sample rate, channel layout, sample format, frame sequence, monotonic capture timestamp/sample index, and a discontinuity marker. A frame's duration is derived from its sample count and sample rate. Do not compare unrelated device wall clocks; establish a monotonic session clock and record clock conversion/drift when a device reports its own clock.

Every callback/event carries a `sessionId`, a stream/node identifier, and a monotonically increasing sequence. Transcript events additionally carry a revision, stable-prefix length (when provided), final/partial state, locale, confidence and optional word timestamps. Playback chunks carry their format and the first sample's position on the playback timeline. Stale session events are ignored at the boundary, not guessed at by text equality.

No callback may be invoked while an internal lock is held. Callback payloads must not borrow memory whose lifetime ends when the adapter returns. Cancellation is idempotent and thread-safe; `stopAndWait` (or equivalent async shutdown completion) is a teardown barrier, not the normal UI stop action. A backend must document which executor invokes callbacks and never synchronously wait on its own callback thread.

### Audio capture and output

`CaptureSource` enumerates stable device IDs and labels, negotiated formats, channel count, and hot-plug changes. It starts asynchronously, emits bounded PCM frames, and reports permission denial, device removal, underruns, and format changes as typed events. `PlaybackSink` accepts timestamped chunks, supports immediate cancel and a short configurable fade, and exposes a playback-reference tap to AEC. Device switching is an explicit reconfiguration transaction: stop or drain, rebuild the graph, reset stateful processors, then publish the new active route.

The audio callback is real-time sensitive: do not allocate unpredictably, block on inference, log synchronously, or acquire a contended application mutex there. Audio callbacks write to a preallocated single-producer/single-consumer ring buffer; worker queues have explicit capacity and overflow policy.

### Processing, VAD, and endpointing

Processors declare accepted input formats, output format, latency, statefulness, and reset behavior. Format conversion happens at graph boundaries; the graph should avoid repeated resampling and channel conversion. Optional denoising and automatic gain control are separate nodes from AEC so each can be benchmarked and switched independently.

VAD consumes timestamped audio and emits speech-start, speech-end, and probability/diagnostic observations. It is not the recognizer and does not own turn policy. Endpointing combines VAD, recognizer end-of-utterance hints, configurable silence duration, and an explicit user submit action. Keep thresholds, pre-roll, hangover, and maximum utterance duration configurable per voice profile, with safe validated bounds. Preserve a short bounded pre-roll so initial consonants are not lost when onset is detected late.

### Acoustic echo cancellation

AEC is a first-class replaceable processor with two synchronized inputs: microphone capture and the exact rendered playback reference. AEC runs before VAD/STT; otherwise Kestrel can transcribe its own synthesized voice and falsely barge in. The graph must handle output latency, sample-rate changes, drift, silence, and reference discontinuities. If playback reference is unavailable or AEC resets, expose that degraded state rather than claiming echo cancellation is active.

AEC is not a universal checkbox: select an implementation only after measuring echo return loss enhancement, double-talk behavior, clipping, CPU cost, and latency on supported devices. The architecture permits OS-provided or bundled DSP implementations, but no specific library is mandated here. Include a bypass mode for diagnosis and accessibility, and make the active AEC path visible in diagnostics.

### Streaming recognition

A `StreamingRecognizer` accepts audio frames incrementally and emits zero or more hypotheses followed by a final result or typed termination. It declares supported locales, sample formats, partial-result behavior, word timing, speaker labels (if any), online/offline status, model requirements, privacy/network behavior, and cancellation quality. Partial hypotheses are revisions, not append-only fragments: consumers replace the unstable suffix and keep the stable prefix. A backend that only returns a final phrase remains a valid lower-capability adapter.

The orchestrator does not pretend a final-only engine is streaming. Provider selection can prefer streaming, offline, a language, or a latency bound; if no adapter satisfies a requested requirement, explain the mismatch and ask before relaxing it.

### Streaming synthesis and playback

A `StreamingSynthesizer` takes text chunks plus locale, voice ID, style/prosody controls, and cancellation token; it emits ordered PCM chunks and a terminal event. It declares whether it supports incremental text, first-audio estimates, voice cloning, SSML/prosody controls, sample rates, and local/network processing. A `SpeechPlanner` buffers enough punctuation/context for natural chunks but starts synthesis before the whole answer is complete.

TTS chunks are tagged with the response/turn ID. If the user interrupts, cancel generation and synthesis for that turn, discard queued stale chunks, and apply a short audio fade. The policy may allow completion of a currently playing phonetic unit only when doing so is faster and safer than an abrupt cut; it must not hold a new response hostage to an arbitrarily long clause. The spoken cursor advances from playback acknowledgements, not from generated or merely synthesized text.

## Session orchestration and full duplex

Use an explicit state machine rather than a collection of loosely coupled booleans. Example states: `Idle`, `Arming`, `Listening`, `Endpointing`, `Thinking`, `Speaking`, `ListeningWhileSpeaking`, `Stopping`, and `Recovering`. Transitions include the triggering event, owning session/turn ID, and cancellation effects. Starting a new user turn invalidates old transcript, LLM, TTS, and playback events atomically from the controller's perspective.

Full duplex keeps capture and AEC alive while the assistant speaks. VAD/STT may run concurrently with playback; AEC removes the assistant reference, and a configurable barge-in policy uses speech confidence, duration, and user settings to pause or cancel output. Separate the acoustic event (speech detected) from intent (interrupt now): avoid both false interruptions from echo and excessive delay when the user clearly takes the floor. Support push-to-talk and half-duplex fallbacks when AEC or device routing is unavailable.

Maintain bounded end-to-end latency rather than unbounded queues. Suggested initial engineering budgets (targets to measure, not guarantees): 10–20 ms audio frames; less than 100 ms steady-state capture-to-VAD event; under 700 ms onset-to-first useful partial on a representative local system; under 1.5 s turn-end-to-first synthesized audio for a warm local path. Publish p50/p95 by device, backend, model, locale, and hardware. A late partial must never delay live audio; replace superseded partials, but never silently drop a final transcript or a playback cancellation.

## Adapter discovery, selection, and configuration

A provider descriptor should report: stable provider ID/version, adapter API version, input/output formats, locales, streaming/partial support, offline/network status, required models and approximate resource needs, measured/estimated latency, cancellation and hot-swap behavior, license/source metadata, and security/privacy declarations. Discovery is separate from instantiation; probing must not open a microphone or download weights without consent.

Selection builds a compatible graph from explicit user preferences and hard constraints (for example local-only, locale, device, streaming partials, and latency ceiling). Rank only candidates that meet hard constraints, show the chosen graph and fallback order, and let the user pin any node independently. Switching one component should not silently switch the others. Persist a versioned profile such as `capture`, `aec`, `vad`, `stt`, `tts`, `playback`, `locale`, `bargeIn`, and `privacy`; validate/migrate settings and never store credentials in the voice profile.

Use in-process adapters for reviewed OS and bundled libraries initially. If third-party engines later run out of process, define a versioned local IPC protocol with bounded messages, explicit process ownership, authentication, crash recovery, and no implicit filesystem/network authority. Do not treat a plugin ABI as a security sandbox.

## Privacy, safety, and failure behavior

- Default to local processing and show a persistent, truthful indicator for microphone capture and active engines.
- Obtain explicit consent before cloud STT/TTS, audio retention, voice cloning, or model downloads. Clearly name where audio is sent and what is retained.
- Do not persist raw audio or transcripts beyond the conversation's configured retention policy. Redact audio payloads, credentials, and sensitive paths from logs.
- Enforce a user-visible microphone mute that stops capture at the source, not just downstream recognition. Provide push-to-talk and a keyboard stop path.
- On device loss, permission revocation, backend crash, model-load failure, or format mismatch: cancel dependent nodes, invalidate the session generation, release devices, report the specific failure, and offer a compatible fallback only within the user's privacy constraints.
- A fallback must never silently cross from local to network processing. Keep transcript text visible/editable before submission when configured, and route recognized speech through the same permissions and safety checks as typed input.

## Testing and observability

Build a deterministic `VirtualAudioSource`, fake clock, fake VAD/STT/TTS, and sink that can inject frames, partial revisions, device loss, callback races, output latency, and cancellation. Tests should prove: stale callbacks cannot mutate a new turn; split/resampled frames preserve timestamps; VAD pre-roll retains initial speech; AEC receives the exact playback reference; final transcripts survive backpressure; barge-in cancels stale LLM/TTS audio; no-audio devices degrade cleanly; and `stopAndWait` is a real lifetime boundary.

Add recorded, consented acoustic fixtures for noise, reverberation, double-talk, echo, clipping, and multiple sample rates. Keep them small, licensed, and non-identifying. Measure WER/CER and endpointing delay for STT; first-audio and real-time factor for TTS; echo reduction and double-talk for AEC; CPU/GPU/memory; p50/p95 latency; and cancellation-to-silence. Report test corpus and hardware so scores are not presented as universal. Unit tests must not require live hardware or network.

The diagnostics panel should expose the selected node/version at every stage, device/format/locale, local-vs-network status, VAD state, AEC active/degraded state, queue depth/overruns, partial/final timing, first-token/first-audio timing, and last error. Diagnostics must be opt-in where they could reveal transcript content.

## Incremental delivery plan

1. **Stabilize contracts:** define common frame, clock, session-ID, capability, error, and cancellation types; adapt the existing `SpeechRecognizer`, `ListenSession`, and `SpeechBackend` behind compatibility wrappers. Keep the current UI behavior working.
2. **Add true streaming seams:** introduce streaming STT and chunked TTS contracts plus fake implementations; demonstrate partial revisions and cancellation in deterministic tests before adding another production engine.
3. **Build the audio graph:** add capture/playback abstractions, device discovery, bounded ring buffers, format conversion, lifecycle and device-loss recovery; ship push-to-talk first.
4. **Add local VAD and AEC:** measure double-talk and latency with the playback reference; expose bypass/degraded states. Do not enable always-on duplex by default until acoustic tests pass.
5. **Enable incremental duplex:** connect AEC → VAD/STT while TTS plays; add turn arbitration, quick interruption fade, and stale-chunk rejection. Retain half-duplex fallback.
6. **Provider selection and profiles:** discover compatible adapters, enforce local-only/privacy constraints, support independent component selection, and migrate existing voice settings.
7. **Evaluate alternatives:** benchmark multiple local STT/TTS/AEC options on supported Windows/Linux/macOS targets, with licensing and packaging review before choosing defaults. Cloud providers, if added, are optional and separately consented.

## Acceptance criteria for calling it advanced

Do not label the runtime advanced/full-duplex until it has independently swappable production STT, VAD, AEC, and streaming TTS adapters; verified simultaneous input/output; measured echo and double-talk behavior; responsive interruption with stale audio eliminated; device-loss and shutdown race tests; truthful offline/network indicators; and published latency/resource measurements on target hardware. The current adapters are a starting seam, not that finished capability.
