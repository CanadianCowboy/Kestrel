# Kestrel tool router

## Delivery status

- The staged router, stage UI, manual override, Base/Persona and Base/Tool adapter selection, and visible/persisted tool trace are implemented and covered by app/controller tests.
- The local Base → test workspace tool → Base answer path was exercised with real GGUF models. The optional Instruct GGUF is installed and passes its pinned size/SHA-256 verifier; it is selected explicitly in the UI.
- Undermind OAuth, discovery, schemas, permission changes, and saved-token reconnect are covered using scripted transport and an in-memory keystore. A live account/server run is still unverified until the user connects their account.
- Automatic Tool-LoRA training uses successful tool-call targets only, separate from Persona examples, and includes its own reviewed seed set. Trainer validation and training-data tests cover stage boundaries; actual Tool-LoRA GPU training has not been run.

The sections below retain the agreed acceptance contract and safety boundaries.

## User decisions

- Keep Base + Persona as Kestrel's normal conversational voice.
- Smart intent triggering plus a manual override selects a two-stage tool router.
- Tool stage: Base + dedicated Tool-LoRA, or an optional instruct-tuned Qwen3 GGUF.
- Answer stage: adaptive Base + Persona or Instruct + Persona for complex tool synthesis.
- Keep only one model resident at a time; model swaps must be visible and cancellable.
- Train dedicated Tool and Persona adapters first, not a mandatory unified adapter.
- Each adapter belongs to its exact parent model. Never silently reuse a Base adapter on Instruct.
- Share explicitly taught preferences through the prompt on both models.
- Automatically collect eligible successful tool traces and distill them when the existing chat-learning consent is enabled; no extra per-trace review requirement. Failed/refused calls are not success examples. Preserve the existing sensitive-data screening and permission boundary.
- Show a live stage pill and persist the stage/tool trace with the conversation.
- Supply a verified model setup script and UI model/mode controls.
- For 'load my workspace from Undermind', use the current workspace only when the server explicitly identifies it. Ask when ambiguous; never invent a workspace, tool name, or result.

## Delivery order and acceptance criteria

1. **Streaming handoff.** A complete supported tagged or leading JSON tool call cancels model generation immediately. Execute only after `generate()` returns and releases its locks. Hide the call and any invented trailing result. Split tokens, nested objects, escaped braces and UTF-8 must be safe. Explicit user cancellation must not execute a captured call.
2. **Conversation durability.** Save user text, assistant text, tool messages, titles, delivery status and failure notes. Restore after restart; interrupted streaming messages become stopped, not complete. Save rename/delete/selection changes. Use the OS profile store where available plus an atomic owner-only `conversations.json` inspection mirror in Qt's app-data directory (`KESTREL_CONVERSATIONS_PATH` overrides it for tests). Report write/parse failures; never silently replace invalid history. Test through controller actions and fresh controllers, not only serialization helpers.
3. **Undermind.** Prioritize its workspace tools within the capability budget. Keep existing permission checks. Verify discovered schemas and current-workspace behavior against the actual server when credentials and a connection are available; report a missing connection as unverified, not success.
4. **Router.** Implement a testable stage state machine, bounded tool rounds, override, adaptive answer handoff, adapter/model compatibility checks, cancellation and safe restoration. Persist stage reasons and tool results without treating remote result text as instructions.
5. **Model setup and learning.** Pin downloads to an immutable revision and verify the expected SHA-256 before installing. Never label a script verified without testing it. Train dedicated artifacts with parent-model/dataset provenance. Distillation must preserve actual tool calls and evidence, not model-invented results.
6. **Verification.** Run C++ build/type checks, relevant CTest suites, Python tests, QML/controller tests, and real-model/server checks when available. Measure timing before claiming an improvement; document unavailable checks and unfinished stages.

## Resource and safety boundaries

The portable C++20 core and runtime remain Qt/vendor-header independent. Keep the mock backend usable. No idle-loop network/filesystem capabilities. No model download during ordinary chat. Existing explicit command/file/MCP permission checks apply equally to both stages. An Instruct model is an optional local backend choice, not a hosted service.
