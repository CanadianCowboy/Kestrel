# Kestrel memory

## Design contract

Memory is Kestrel-owned and local; no hosted memory service, vector database,
embedding API, or extra model request is required. This implementation is an
explainable baseline, not a claim to beat external benchmarks.

- Fresh profiles enable automatic memory when a usable OS keystore is attached.
  Existing profiles retain their saved on/off choice. Without a working keystore,
  memory remains unavailable. No plaintext fallback. Disabling recall immediately
  stops capture and retrieval even if saving that choice fails; report the failure.
- Automatically keep **user messages as episodic observations**, never as verified
  facts. Do not promote assistant text, web pages, or tool output into user facts.
  This is verbatim capture, not a model extracting or inventing facts. Manual
  memories are user-confirmed facts. Updates remain explicit and editable.
- Sensitive-content screening is conservative and best-effort, not a guarantee.
  Recognized sensitive user messages require approval before persistence. Pending
  proposals exist only in process memory, are bounded, and disappear on opt-out,
  clear, or restart. Approved sensitive memories are local-only and are never
  automatically retrieved into a cloud request. Text already typed into a cloud
  chat is still sent to its selected provider; local-only memory does not redact
  that chat message or earlier chat history. The manual note field also has a
  **Private / local only** checkbox to request protection regardless of screening.
- Each record has stable ID, kind, created/updated timestamps, source label,
  sensitivity, and an approved/pending boundary (pending items never persist).
  Exact normalized duplicates reuse a record;
  explicit edits replace it in place, with the newest text authoritative. No
  guessed semantic conflict resolution and no silent eviction.
- Query-aware local lexical ranking selects relevant records instead of merely
  the newest ones. Explicitly saved facts can supply a small background profile.
  Retrieval may return no matches. It must not infer that absent evidence is a
  remembered fact. Lexical retrieval is not embedding-based semantic search.
- Bound prompt memory to 2,400 UTF-8 bytes and eight records. Skip items that do
  not fit instead of stopping at the first oversized item. Mark retrieved content
  as quoted data with IDs, timestamps, and source, not system instructions. Keep
  the shared system prefix stable. Show the last retrieval in the Memory panel.
- Bound stored memories to 2,000 records; each record is at most 500 UTF-8 bytes.
  Automatic capture accepts messages up to 2,000 UTF-8 bytes, split at character
  boundaries. Larger messages are skipped with a visible notice instead of
  silently keeping only a misleading prefix. Pending sensitive messages are
  capped at 20. Capacity failures are visible and never silently evict records.
- Every mutation uses the existing encrypted profile and rolls back on save
  failure, except stopping recall (a privacy stop must take effect immediately).
  Delete/clear remove the persistent records and pending approvals. The current
  conversation may still contain the original text; forgetting a memory does not
  delete a transcript or a provider's already-received data.

## Interfaces and migration

Extend the existing `src/core/recall.*`, `src/storage/profile.*`,
`src/app/appcontroller.*`, and Memory panel in `src/ui/Main.qml`. Preserve the
existing `rememberFact`, `forgetMemory`, `clearMemories`, and search interfaces;
add approval, edit, source/updated metadata, capture notice, and last-retrieval
properties/actions. Profile format v4 adds separately reviewed training examples
(see [LEARNING.md](LEARNING.md)) and accepts v1/v2/v3 while preserving IDs,
other profile fields, and explicit opt-outs. A newer or malformed profile must
never be overwritten. Core time remains supplied by its owner.

## Evaluation

Test relevance over recency, no-match abstention, duplicates, corrections,
sensitive approval/rejection and cloud exclusion, opt-out, restart/migration,
save-failure rollback, corrupted-profile refusal, UTF-8 boundaries, prompt limits,
and real QML controls. Measure retrieval timing and correctness on a synthetic
2,000-record corpus; report measurements, not an unmeasured speedup. These are
local regression cases inspired by the research, not a LongMemEval score.

## Research informing the design

- [LongMemEval (ICLR 2025)](https://arxiv.org/abs/2410.10813): separate indexing,
  retrieval, and reading; evaluate extraction, cross-session reasoning, time,
  knowledge updates, and abstention. More history alone is not reliable memory.
- [LangChain memory concepts](https://docs.langchain.com/oss/python/concepts/memory):
  distinguish semantic facts, episodic observations, and procedural instructions;
  collections simplify writes but require deliberate update/retrieval behavior.
- [Letta memory benchmarking](https://www.letta.com/blog/benchmarking-ai-agent-memory/):
  evaluate the whole model/tool/context pipeline, not a retrieval component in
  isolation. Vendor benchmark claims are not independent comparative evidence.

## Initial local measurement

On the Linux development machine, the dependency-free regression corpus of 2,000
synthetic episodic records returned the requested exact topic first in 30/30
distinct-topic queries (median 8.67 ms, maximum 8.90 ms in that run). The unrelated no-match case
returned no records. These exact-token synthetic queries do not establish
paraphrase recall, end-to-end model accuracy, or a speedup over another system.

Later extensions should be justified by evaluation: local embeddings for paraphrase
recall, user-reviewed fact extraction and topic summaries, project scopes, temporal
validity/supersession history, and independently measured end-to-end answer quality.
