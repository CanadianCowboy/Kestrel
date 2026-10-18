# Automatic local behavior learning

Kestrel is local-only. Groq configuration and cloud-selection controls are no
longer available; old cloud configuration calls are refused without reading keys.
Remote MCP tools remain explicitly connected network integrations, not inference
backends. Remote results are not training targets; successful calls may contribute
the executed call itself to the separate Tool-stage dataset with learning consent.

## User workflow

1. Launch Kestrel. It loads the last working local model, otherwise discovers
   installed models beside the executable, in application-data `models/`, or in
   the development checkout's `build/models/`. `--model` and `KESTREL_MODEL` remain
   explicit overrides. Missing models produce an installation diagnostic, not a
   cloud or mock fallback. No automatic download occurs.
2. Open **Memory (Ctrl+M)** and enable **Learn automatically from my conversations
   (local only)** once. This consent is saved in the encrypted OS-keystore
   profile. Old profiles do not opt in during migration. Without a working
   keystore, automatic learning cannot be enabled.
3. Just chat. Completed, non-sensitive, non-tool-assisted turns become local
   examples after consent. Stopped/failed replies, tool outputs, oversized turns,
   and recognized sensitive text are excluded. Screening is best-effort, not a
   guarantee that no personal information enters weights.
4. Teach behavior naturally. For example, `From now on keep answers short` saves
   a user-authored preference for immediate prompting, including across restarts.
   `When I ask status, say Ready.` adds a concrete prompt/answer example. These
   preferences do not grant tool capabilities or administrator permission. The
   Memory panel lists taught preferences with individual **Forget preference**
   actions. `When I ask status, say All clear.` replaces an earlier target for
   `status`, ignoring case and repeated whitespace. `No, say …` teaches a target
   for the preceding user request. Target/preference updates save atomically;
   failed secure saves leave the previous behavior intact.
5. With at least eight distinct usable prompts, after 60 seconds without chat
   activity, the app automatically trains a candidate if inference, dictation,
   speech playback and resumable output are idle. Inference VRAM is released;
   the model reloads afterward. Status explains collection, training, conversion,
   activation or rejection. Returning during training stops the local process;
   retry the message once model restoration finishes.
6. Disable learning to stop future collection/training. Existing learned behavior
   remains available. **Reset learned behavior** confirms before clearing
   examples/preferences, removing the active adapter and its startup selection,
   and deleting managed learning artifacts. Previously exported files remain.

Ordinary assistant replies are self-distillation targets, not proof that those
replies are correct or preferred. Chat corrections and teaching are important.
Deleting a chat/example cannot reliably erase information already trained into
an adapter. Reset to base weights is the reliable local rollback boundary.

Manual example review/export is optional under **Advanced review**, not required
for ordinary teaching. Export preserves whether each example was reviewed or
captured under chat-learning consent; auto-captured and chat-taught examples are
never falsely marked reviewed. JSONL exports retain `source` (`chat`, `teaching`,
`reviewed`) and explicit review/consent flags. Standalone training refuses
non-reviewed exports unless `--allow-consented-chat` is given. Model and LoRA file
management are not the normal user workflow.

## Automatic local lifecycle

The app-owned `AutomaticLearning` coordinator runs only the installed Python
trainer and compatible llama.cpp converter, directly through QProcess (no shell).
It is **not IdlePersona work**: the sandboxed core idle loop has not gained file,
network or command capability. One-time learning consent covers this separate
local GPU/file workflow. No services, uploads or remote model code are used.

- Automatic weight training currently targets `Qwen/Qwen3-4B-Base`; both Persona and Tool adapters are distinct and bound to that lineage. Instruct GGUF is an optional inference/routing model and must never receive a Base LoRA.
- The optional Instruct artifact is installed only by `python3 scripts/setup_instruct.py`. It pins an immutable publisher revision and verifies exact file size and SHA-256 before writing the adjacent manifest. It has been verified locally; setup is not run during ordinary chat.
- Inference artifact: `Qwen3-4B-Base.Q4_K_M.gguf`; its adjacent
  `Qwen3-4B-Base.Q4_K_M.manifest.json` records base identity, filename, bytes and
  SHA-256. The trainer verifies the inference file and installed training files
  against manifests before starting GPU work.
- The local NF4 training directory is `Qwen3-4B-Base-bnb-4bit` with its
  `download-manifest.json`; conversion uses original non-quantized base
  configuration in `Qwen3-4B-Base-config/`.
- Batch size 1, rank 4, Q/V projections, eight steps, maximum 512 tokens per
  example. Overlength examples are reported and skipped, never truncated.
- Automatic Persona runs target `--domain astronomy` and merge the packaged,
  reviewed `scripts/astronomy_seed.jsonl` corpus. Automatic Tool runs use only
  successful executed-call targets and merge the stage-scoped reviewed
  `scripts/tool_seed.jsonl` protocol examples; these are not mixed with Persona targets. A seed never overrides the
  user's own answer for the same prompt, and the merge does not lower the
  eight-example collection gate: the seeds keep the field in the adapter, they
  do not trigger training on their own. Curation reserves at least half of the
  replay budget for astronomy, so a run of generic corrections cannot train the
  specialism out. See [ASTRONOMY.md](ASTRONOMY.md).
- Automatic curation keeps at most 128 distinct normalized prompts, including
  at most 32 self-generated chat replies. Explicit teaching/review overrides
  chat and packaged seeds; the latest explicit correction wins. Recent
  revisions are kept when the budget fills. Explicit targets receive two
  training copies versus one for chat, only after splitting so weighting cannot
  leak targets into validation.
- The desktop never imports external datasets. `--allow-imported` on a manual
  run accepts `source: "imported"` examples that carry a licence and origin,
  and the activation gate then also requires the astronomy-held-out probe not to
  regress. Imported examples are never marked reviewed.
- Case/whitespace-normalized, prompt-disjoint 20% holdout. Activation requires
  finite improved held-out loss,
  nonempty/non-tool outputs, no regression on a small deterministic held-out
  text-similarity probe, and, for a specialist run, no regression on the
  astronomy-only probe. These are limited automated checks, **not proof of
  general chat quality, factual reliability, privacy or tool safety**.
- GPU memory admission requires at least 3.5 GiB free; training has a five-minute
  process deadline. Inference is restored after cancellation, failure or rejection.
  Failed datasets do not cause continuous automatic retries.
- Candidate PEFT weights, tokenizer copies and temporary owner-only plaintext
  JSONL are removed at completion. Accepted owner-only GGUF adapters live under
  application-data `learning/`; the working adapter/model pairing reloads on the
  next launch. Failed activation restores the preceding adapter. Roll back
  learned weights returns to base weights without deleting editable memory.
- Owner-only `learning/last-evaluation.json` keeps the latest completed candidate's
  loss, behavior metrics, curation counts, seed and hashed held-out prompt IDs.
  Accepted adapters also retain an adjacent `.evaluation.json`. Reports omit raw
  prompts, preferred answers and generated outputs; fingerprints are identifiers,
  not a guarantee of anonymity. The Memory panel shows the evaluation summary;
  passing training checks and actual activation are reported separately.
- Profile v6 stores opt-in, source-tagged examples and up to 32 user-taught
  preferences. Secure-save failures roll back mutations. At most 1000 examples,
  each field at most 4000 UTF-8 bytes; each preference at most 500 bytes.

The normal installed build must package local training assets. Discovery checks
app/development/user-data roots for `training-venv`, `scripts/train_persona.py`,
`scripts/astronomy_seed.jsonl` (it is resolved beside the trainer),
`models/Qwen3-4B-Base-bnb-4bit`, and `models/Qwen3-4B-Base-config`. The converter
is discovered at `training/convert_lora_to_gguf.py` beside the app,
`third_party/llama.cpp/convert_lora_to_gguf.py` in the checkout, or the existing
local developer installation `~/.local/src/llama.cpp/`. Ship converter support
modules from the compatible llama.cpp version as well. Missing assets keep chat
capture/preferences working and produce an honest diagnostic; the app never
silently downloads multi-gigabyte training dependencies.

The development checkout already has the local GGUF, NF4 weights, configuration,
training environment and converter. Training packages are pinned in
`scripts/training-requirements.txt`; no extra ML dependency is added to the C++
portable core. Windows packaging must provide the equivalent local training
runtime; CUDA/Python package availability is not inferred from GGUF inference.

## Verification

```sh
cmake --build build-linux -j 2
ctest --test-dir build-linux --output-on-failure
python3 -m unittest discover -s tests -p train_persona_tests.py -v
./build-linux/kestrel --print-runtime
QT_QPA_PLATFORM=offscreen ./build-linux/kestrel --smoke-test
```

Opt-in real GPU lifecycle test (uses isolated temporary configuration, data and
an in-memory keystore; never trains on the developer's conversations):

```sh
QT_QPA_PLATFORM=offscreen \
KESTREL_TEST_AUTO_LEARNING_MODEL="$PWD/build/models/Qwen3-4B-Base.Q4_K_M.gguf" \
KESTREL_TEST_AUTO_LEARNING_DATASET="$PWD/build/learning-smoke.jsonl" \
./build-linux/kestrel_app_tests
```

The synthetic eight-example dataset verifies mechanics, not useful personality.
A local eight-step run measured held-out loss 2.29668 → 2.19364, text-similarity
probe 0.22476 → 0.27255, ~3.92 GiB peak allocated GPU memory, and 13.55 seconds
for model loading/training/evaluation (excluding package imports and checksum
verification). A subsequent run with curation/report retention also passed actual
activation, protected evaluation artifacts, an invalid-replacement check, a real
completed chat reply with learned weights, and reset. It measured the same
held-out losses, ~3.92 GiB peak allocation and 8.78 seconds within the trainer's
loading/training/evaluation window. These separate timings are not a speedup
comparison. An initial mixed-precision generation probe failed and was fixed
with BF16 autocast; it is not counted as a pass.

## Hands-on memory and teaching check

Launch `./build-linux/kestrel`, then open **Memory (Ctrl+M)**:

1. Save the same ordinary note twice with different casing/spacing. It remains
   one record and its reinforcement count increases. Pin it, then unpin it.
2. Save an updated note, enter its ID on the old note and choose **Supersede**.
   Both remain inspectable, but only the replacement is eligible for recall.
3. Enable local learning and chat `From now on keep answers short`, then
   `When I ask status, say Ready.` and `When I ask STATUS, say All clear.`
   The listed specific preference and training target should contain only the
   latest answer. Restart to verify persistence.
4. Remove one preference through **Forget preference**. Its prompt instruction
   disappears; existing adapter weights do not change. Eight usable distinct
   examples plus an idle interval exercise automatic training. Inspect both
   status and the last-evaluation summary; a rejection must leave chat usable.
5. Use **Reset learned behavior** to return to base weights and clear managed
   examples/preferences/artifacts. This does not clear editable memories or
   previously exported datasets.

The base model has limited chat/instruction-following ability. Persistent
instructions and passing small automated probes do not establish reliable
compliance with every preference, factual correctness, or general quality.

MCP implementation scope and a hands-on endpoint test are in
[`mcp/manual-test.md`](mcp/manual-test.md). Automated Undermind tests use a
scripted server and in-memory keystore; they do not verify your account or current
server schemas. Use the Activity panel's **Connect Undermind** action for that
live check. If no saved keyring grant exists, Kestrel opens the OAuth browser
flow; approve the requested Undermind access there.
