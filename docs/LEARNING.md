# Local base-model behavior learning

## Model and limits

The selected inference artifact is `Qwen3-4B-Base.Q4_K_M.gguf`, from
[mradermacher/Qwen3-4B-Base-GGUF](https://huggingface.co/mradermacher/Qwen3-4B-Base-GGUF),
a quantization of [Qwen/Qwen3-4B-Base](https://huggingface.co/Qwen/Qwen3-4B-Base)
(Apache 2.0). It is **not** Qwen3-4B chat or Instruct-2507.

Local installation for this checkout: `build/models/Qwen3-4B-Base.Q4_K_M.gguf`.
The adjacent manifest records the pinned revision, byte count and SHA-256 verified
against the repository's LFS metadata. Build/model artifacts are ignored by git.

Kestrel uses plain transcript prompting for GGUFs marked `general.finetune=Base`,
even if their tokenizer metadata includes a chat template. That metadata does not
mean base weights were chat-trained. The same `user: ...\nassistant:` format is
used by the training CLI. Base output stops before a generated next-speaker label;
this prevents invented transcript turns, not hallucinations within an answer.

The default context is capped at 4096 tokens. Qwen3 KV accounting uses its explicit
128-dimensional K/V heads rather than `hidden_size / query_heads` (80 for 4B).
The latter underestimated memory and caused a 21K-context allocation to fail on
the RTX 4050. A 4K Qwen3 4B FP16 cache is 576 MiB. This is not a guarantee that
every model fits, or a quality claim about a base model's chat/tool behavior.

## Memory and reviewed corrections are separate

Memory remains the existing editable, encrypted local recall system. It does not
modify model weights. Do not train private personal facts: deletion from memory
or a dataset cannot reliably erase information already learned into an adapter.

Open **Memory (Ctrl+M) → Behavior learning**:

1. Review the latest chat answer or enter a prompt manually.
2. Edit the original answer and write the preferred answer. Remove private facts,
   credentials and tool output. The original answer is review context, **not** a
   positive training target.
3. Check the explicit review checkbox and save. Only this action adds an example;
   normal chats, recall records, and idle work never become training data.
4. Inspect/delete saved examples. They persist in the OS-keystore profile, format
   v4 (reads v1–v3 without inventing examples or changing saved memory opt-outs).
5. Explicitly export reviewed JSONL. Export is plaintext, owner-only on POSIX,
   local, atomic, and does not upload, train, or activate anything. Keep exports
   under ignored `data/`, not in source control.

Sensitive-content screening is best-effort and not a guarantee. Recognized
sensitive text is refused for training capture. At most 1000 reviewed examples
are stored, each text field at most 4000 UTF-8 bytes. Invalid/oversized data is
refused rather than silently truncated. Secure-save failure rolls back mutations.
Deleting stored examples does not remove earlier exports or untrain adapters.

Export schema (one JSON object per UTF-8 line):

```json
{"schema_version":1,"reviewed":true,"id":1,"base_model":"Qwen/Qwen3-4B-Base","prompt":"Say hello","rejected":"Formal greeting","preferred":"Hey, good to see you."}
```

## Local QLoRA training

Training requires more software and memory than GGUF inference. It is a separate,
explicit CLI, **never an idle task**, and never uploads datasets. The GPU should
not run inference while training. On a busy 16 GB / 6 GB laptop, loading may still
fail; close other memory-heavy work instead of assuming quantization guarantees
fit. A handful of examples tests the mechanics, not a useful personality.

```bash
python3 -m venv build/training-venv
build/training-venv/bin/python -m pip install -r scripts/training-requirements.txt
python3 scripts/train_persona.py --dataset data/reviewed.jsonl --validate-only
```

The CLI accepts only a **local Hugging Face model directory**, not a remote ID or
GGUF. A local copy of
[unsloth/Qwen3-4B-Base-bnb-4bit](https://huggingface.co/unsloth/Qwen3-4B-Base-bnb-4bit)
contains the same base lineage in NF4 form, so it avoids downloading/loading full
BF16 weights on a RAM-constrained machine. This is a weights source, not a hosted
training service. Download it separately, verify provenance/checksums, and keep
its manifest. No remote model code is trusted.

```bash
build/training-venv/bin/python scripts/train_persona.py \
  --dataset data/reviewed.jsonl \
  --model build/models/Qwen3-4B-Base-bnb-4bit \
  --output data/adapters/persona-v1 \
  --steps 8 --max-length 128 --rank 4 --approve-training
```

The output directory must be new; existing weights/adapters are not overwritten.
The script sets Hugging Face offline/telemetry flags and disables experiment
trackers. The frozen tied vocabulary/output matrix stays BF16 instead of PEFT's
FP32 default, which otherwise exhausted this GPU during evaluation. Norms remain
FP32. It uses NF4, batch size 1, gradient checkpointing, small Q/V-projection
LoRA adapters, assistant-answer-only loss, and no silent truncation. Rejected
answers are not trained. Prompt groups are disjoint between training and the
seeded 20% holdout split. At least two distinct prompts are required.

Output: standard PEFT `adapter_config.json`, `adapter_model.safetensors`, tokenizer
files, and `evaluation.json` with dataset hash, before/after held-out loss, run
time, peak GPU allocation and `activated=false`. Lower loss is not demonstrated
chat quality. Review held-out answers, multi-turn behavior, factual reliability,
privacy, and tools before using an adapter. Small corrections alone will not
teach every chat/tool skill missing from a base model.

## Convert, activate, and roll back

Use the converter from the **same compatible llama.cpp source installation** as
your runtime, with its documented dependencies. Keep conversion offline by
passing a local directory containing the **original Qwen base `config.json`**
(no `--trust-remote-code`). Do not pass the bitsandbytes training directory: this
converter rejects its quantization metadata even when exporting adapter-only
weights. The small original configuration is sufficient; full BF16 weights are
not needed for adapter conversion. In this checkout the verified original
configuration and revision are in `build/models/Qwen3-4B-Base-config/`:


```bash
build/training-venv/bin/python /path/to/llama.cpp/convert_lora_to_gguf.py \
  --base build/models/Qwen3-4B-Base-config \
  --outfile data/adapters/persona-v1.gguf --outtype f16 data/adapters/persona-v1
```

In Memory → Behavior learning, choose **Load GGUF LoRA…** only after evaluation.
Use an adapter for this exact base lineage; matching tensor shapes alone do not
prove provenance. Activation is session-only and refused during generation. A
failed adapter load preserves the existing adapter. The runtime status names an
active adapter. **Remove LoRA** clears its KV state and restores unchanged base
weights. Start a fresh conversation for comparisons; old conversation text is
not erased by changing adapters.

## Tests

```bash
python3 -m unittest discover -s tests -p train_persona_tests.py -v
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
KESTREL_TEST_GGUF="$PWD/build/models/Qwen3-4B-Base.Q4_K_M.gguf" \
  ctest --test-dir build -R kestrel_runtime_tests --output-on-failure
QT_QPA_PLATFORM=offscreen build/kestrel \
  --model "$PWD/build/models/Qwen3-4B-Base.Q4_K_M.gguf" --smoke-test
```

C++ coverage includes encrypted correction persistence, rollback, migration,
real QML review controls, owner-only JSONL export, no network export, base-model
cache accounting and token-boundary-aware next-speaker filtering. Python tests
cover strict review/base identity, no duplicate or malformed examples, disjoint
holdout, assistant-only labels and no silent token truncation.

## Measured local smoke test

The final UI smoke generated a single assistant turn at 22.1 streamed chunks/sec
(the app metric; not directly comparable to another tokenizer/runtime). All
37 model layers offloaded, with 4096 context and a 576 MiB KV allocation. An
earlier uncapped run failed context allocation; this was repaired, not counted
as success.

A deliberately synthetic eight-example mechanics dataset (six train, two held
out) completed two QLoRA steps with rank 4 / 128-token limit. Peak PyTorch GPU
allocation was 4,209,388,544 bytes (~3.92 GiB); held-out loss changed from 2.29668
to 2.26585. Reported model load + train + evaluation time was 3.20 seconds in that
warm run, excluding downloads/imports. This is not useful personality training
or evidence of better conversations. The initial FP32-embedding run failed OOM;
the BF16 frozen-matrix repair succeeded. The test adapter was converted to a
2.9 MB GGUF but is **not enabled by default**. Its artifacts and evaluation
remain under ignored `build/learning-smoke-adapter-bf16*`.

## Undermind authentication

Explicit **Connect Undermind** always starts a new PKCE browser authorization,
even with saved credentials. Startup can still resume a saved OAuth grant, but
must complete authenticated initialization and tool discovery before reporting
connected. HTTP/authentication failures and expired sessions disable advertised
tools. OAuth browser approval may be quick if the provider already has an active
browser session; that is distinct from Kestrel skipping its authorization flow.
Live account authorization still requires an interactive user check.
