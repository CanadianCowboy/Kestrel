# Consolidation baseline

This document records the branch audit and the intended baseline after the 2026-09-30 integration review. The consolidation branch is a review branch only: changes reach `main` through a GitHub pull request and external review. Do not merge or push directly to `main`.

## Branch and worktree inventory

The audit covered 19 original local branches, 9 worktrees, the published remote branches, and 5 worktrees with uncommitted changes. The original dirty worktrees were left untouched. Before any later cleanup, confirm all names/SHAs against `git branch --all` and `git worktree list`; do not delete a recovery reference or worktree merely because it is not part of the baseline.

At the audit snapshot:

| Ref | Snapshot | Disposition |
| --- | --- | --- |
| `main`, `origin/main` | `66f4cd3` | Protected baseline; unchanged by this work. |
| `integration/all-branches-review` | `d4ce0e2` before consolidation edits | Working branch for the proposed combined baseline. |
| `feature/conversation-models` | `e4f4a52` | Already represented in main. |
| `feature/context-caching-and-model-picker` | `572e405` | Already represented in main. |
| `feature/private-store` | `4c7985f` | Already represented in main. |
| `feature/profile-document` | `38bbe0d` | Already represented in main. |
| `feature/smoke-test` | `348cf4e` | Published smoke-test work already represented in main. |
| `feature/chat-template` | `79e270d` | Selectively integrated: structured chat messages and model-template rendering. |
| `feature/persona-voice-and-dictation` | `1e414d1` | Newer voice branch is the source of the voice baseline; older voice PR branch is superseded, not deleted. |
| `fix/chat-bubbles-and-smoke-timing` | `7ee0a92` local / `1221b56` published | Published UI/smoke fix is in main; preserve the local stronger path-roundtrip and CodeRabbit YAML tests when integrating. |
| `freebuff/your-focus-is-a-linux-ver-...` | `b05c064` | Linux support retained in `d4ce0e2`, including install metadata and CI. |
| `freebuff/fix-pr-10-issues-with-coderabbit-...` | `b05c064` | Recovery copy of the Linux review work. |
| `recovery/2026-09-30-chat-storage-copy` | `76fc1cd` | Recovery reference for previously unpublished chat/storage changes. |
| `recovery/2026-09-30-chat-storage-profile` | `51e19d9` | Recovery reference for profile/document changes. |
| `recovery/2026-09-30-early-cuda` | `d020643` | Recovery reference; inspect before pruning. |
| `recovery/2026-09-30-linux-install-notes` | `1303185` | Recovery reference for install/packaging work. |
| `recovery/2026-09-30-voice-discovery-layout` | `4222043` | Recovery reference for voice discovery and UI changes. |
| `fix/llamacpp-availability-and-model-flag` | `7531423` | Reviewed during the branch audit; relevant behavior is selectively represented. |
| `feature/llamacpp-backend` | `6d0d8bd` | Native backend baseline, subsequently superseded by the integrated adapter and fixes. |

Other Freebuff branches were audited as alternate task work, duplicated forks, or clean copies of `main`; they remain available until an owner explicitly approves cleanup. At the time of this document, GitHub PRs #10 (Linux), #11 (older voice), and #12 (newer voice) were still open; #12 had requested changes. The consolidation must not claim those reviews passed or close those PRs without approval.

## Baseline capabilities

- Qt 6 desktop app with local persistent profile/conversation storage, conversation selection/rename/delete, search, model selection, streaming output, and runtime diagnostics.
- `llama.cpp` GGUF inference when built with the optional dependency; optional ONNX Runtime GenAI folder loading. The default preview backend remains usable when native inference is unavailable.
- CUDA discovery degrades to a portable stub when the toolkit is not installed. TensorRT support is not an interactive inference backend: the current engine tool validates/records engine metadata and must not be described as model conversion or runtime execution.
- Structured conversation messages are rendered by each model-specific chat template; a plain-text fallback remains available where the runtime has no template metadata.
- Speech input is behind `SpeechRecognizer`: Windows SAPI 5 is the current capture adapter; the scripted recognizer is test/preview-only and is not advertised as a real microphone.
- Speech synthesis is behind `SpeechBackend`: Qt Text-to-Speech is the platform path, with an optional Kokoro ONNX subprocess using Qt Multimedia playback. Voice timing is clause-based, not full-duplex.
- Permissioned idle tools are opt-in and local. They do not constitute general-purpose shell or filesystem agent execution.

## Voice architecture decision

Keep the current adapter seams and avoid selecting one exclusive advanced runtime until latency, language, hardware, licensing, and interruption requirements are measured. A future voice pipeline may combine separate capture, VAD, streaming STT, turn manager, inference, streaming TTS, playback, and acoustic echo cancellation implementations. The current path does **not** provide low-latency full duplex, acoustic echo cancellation, continuous streaming SAPI partials, or true word-level interruption. SAPI delivers completed phrases, and current speech barge-in lets an active clause finish. These are known limitations, not completed features.

## Validation snapshot

The preceding integration work passed a Windows MSVC Release Qt desktop build and all 8 CTest suites; an optional real GGUF runtime test and UI smoke test also passed. A separate no-CUDA/no-llama ONNX GenAI native build passed its 6 portable tests. ONNX generation itself was not exercised because no compatible GenAI model was available. These results predate the final SAPI teardown/session-ID changes in this working tree and must be rerun before this baseline is proposed for review. GitHub CI remains the cross-platform authority for Windows/Linux/macOS.
