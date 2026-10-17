#!/usr/bin/env python3
"""Train a local Qwen3 Base behavior adapter from explicitly reviewed JSONL.

No automatic chat capture, uploads, remote code, or adapter activation. Download
model files separately; training accepts only a local model directory.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import random
import sys
import time

BASE_MODEL = "Qwen/Qwen3-4B-Base"


def load_examples(path: Path) -> list[dict]:
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("Dataset exceeds 16 MiB; export at most 1000 reviewed examples.")
    examples = []
    seen = set()
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        record = json.loads(line)
        if not isinstance(record, dict) or record.get("schema_version") != 1 or record.get("reviewed") is not True:
            raise ValueError(f"Line {line_number}: only explicitly reviewed schema_version 1 examples are allowed.")
        if record.get("base_model") != BASE_MODEL:
            raise ValueError(f"Line {line_number}: dataset must target {BASE_MODEL}.")
        for field in ("prompt", "preferred", "rejected"):
            text = record.get(field, "")
            if not isinstance(text, str) or len(text.encode("utf-8")) > 4000 or (field != "rejected" and not text.strip()):
                raise ValueError(f"Line {line_number}: invalid {field}; use nonempty text up to 4000 UTF-8 bytes.")
        key = (record["prompt"].strip(), record["preferred"].strip())
        if key in seen:
            raise ValueError(f"Line {line_number}: duplicate prompt/preferred pair.")
        seen.add(key)
        examples.append(record)
    if not 2 <= len(examples) <= 1000:
        raise ValueError("Use 2–1000 distinct reviewed examples; at least one must be held out for evaluation.")
    # Identical prompts must never occur on both sides of the split.
    if len({entry["prompt"].strip() for entry in examples}) < 2:
        raise ValueError("Use at least two distinct prompts to keep validation separate from training.")
    return examples


def split_examples(examples: list[dict], seed: int = 42) -> tuple[list[dict], list[dict]]:
    prompts = sorted({entry["prompt"].strip() for entry in examples})
    random.Random(seed).shuffle(prompts)
    count = max(1, math.ceil(len(prompts) * 0.2))
    held_out = set(prompts[:count])
    return ([entry for entry in examples if entry["prompt"].strip() not in held_out],
            [entry for entry in examples if entry["prompt"].strip() in held_out])


def encode_example(tokenizer, example: dict, max_length: int) -> dict:
    # A base model has no trained chat interface. Use the same plain format as
    # Kestrel's renderPlainChat, and teach only the preferred assistant suffix.
    prefix = f"user: {example['prompt'].strip()}\nassistant:"
    prompt_ids = tokenizer.encode(prefix, add_special_tokens=False)
    answer_ids = tokenizer.encode(" " + example["preferred"].strip(), add_special_tokens=False)
    input_ids = prompt_ids + answer_ids + [tokenizer.eos_token_id]
    if len(input_ids) > max_length:
        raise ValueError("An example exceeds --max-length; shorten it or explicitly increase the limit. No text was truncated.")
    return {"input_ids": input_ids, "attention_mask": [1] * len(input_ids),
            "labels": [-100] * len(prompt_ids) + answer_ids + [tokenizer.eos_token_id]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--model", type=Path, help="Local Qwen3-4B-Base HF directory, preferably NF4 bitsandbytes; not a GGUF")
    parser.add_argument("--output", type=Path, help="New adapter directory (must not exist)")
    parser.add_argument("--validate-only", action="store_true", help="Validate and summarize without importing training libraries")
    parser.add_argument("--steps", type=int, default=8)
    parser.add_argument("--max-length", type=int, default=128)
    parser.add_argument("--rank", type=int, default=4)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--approve-training", action="store_true", help="Explicitly authorize this GPU training run")
    args = parser.parse_args()
    examples = load_examples(args.dataset)
    train, validation = split_examples(examples, args.seed)
    summary = {"examples": len(examples), "train": len(train), "validation": len(validation),
               "dataset_sha256": hashlib.sha256(args.dataset.read_bytes()).hexdigest(), "base_model": BASE_MODEL}
    if args.validate_only:
        print(json.dumps(summary, indent=2))
        return 0
    if not args.approve_training:
        raise ValueError("No training started. Review the dataset, then pass --approve-training explicitly.")
    if args.model is None or not args.model.is_dir() or args.output is None:
        raise ValueError("Choose a local --model directory and a new --output directory.")
    if args.output.exists():
        raise ValueError("Output already exists. Choose a new directory; existing adapters are never overwritten.")
    if not 1 <= args.steps <= 1000 or not 32 <= args.max_length <= 2048 or not 1 <= args.rank <= 32:
        raise ValueError("Use steps 1–1000, max-length 32–2048 and rank 1–32; start small on a 6 GB GPU.")
    config = json.loads((args.model / "config.json").read_text())
    if config.get("model_type") != "qwen3" or config.get("num_hidden_layers") != 36 or config.get("hidden_size") != 2560:
        raise ValueError("Expected the Qwen3 4B architecture. Verify local model provenance before training.")
    # No repository IDs or remote model code. Installed libraries must remain
    # offline as well, including optional experiment trackers.
    os.environ.update(HF_HUB_OFFLINE="1", TRANSFORMERS_OFFLINE="1", HF_HUB_DISABLE_TELEMETRY="1", WANDB_DISABLED="true")
    import torch
    from peft import LoraConfig, get_peft_model, prepare_model_for_kbit_training
    from transformers import AutoModelForCausalLM, AutoTokenizer, BitsAndBytesConfig, Trainer, TrainingArguments, set_seed
    if not torch.cuda.is_available():
        raise ValueError("CUDA PyTorch is unavailable. Install a CUDA-compatible training environment; no CPU fallback was started.")
    free_bytes, _ = torch.cuda.mem_get_info()
    if free_bytes < 3.5 * 1024**3:
        raise ValueError("Less than 3.5 GiB VRAM is free. Close local inference and other GPU work, then retry.")
    set_seed(args.seed)
    torch.set_num_threads(2)
    tokenizer = AutoTokenizer.from_pretrained(args.model, local_files_only=True, trust_remote_code=False)
    tokenizer.pad_token = tokenizer.eos_token
    train_tokens = [encode_example(tokenizer, item, args.max_length) for item in train]
    validation_tokens = [encode_example(tokenizer, item, args.max_length) for item in validation]
    quantization = BitsAndBytesConfig(load_in_4bit=True, bnb_4bit_quant_type="nf4",
        bnb_4bit_use_double_quant=True, bnb_4bit_compute_dtype=torch.bfloat16)
    started = time.monotonic()
    model = AutoModelForCausalLM.from_pretrained(args.model, local_files_only=True, trust_remote_code=False,
        quantization_config=quantization, device_map={"": 0}, dtype=torch.bfloat16, attn_implementation="sdpa")
    model.config.use_cache = False
    model = prepare_model_for_kbit_training(model, use_gradient_checkpointing=True)
    # PEFT upcasts nonquantized weights to FP32. Qwen3 ties its huge vocabulary
    # embedding and output matrix; retaining this frozen matrix in BF16 saves
    # ~742 MiB while norms remain FP32. It is not an adapter training target.
    model.get_input_embeddings().to(dtype=torch.bfloat16)
    model.get_output_embeddings().to(dtype=torch.bfloat16)
    model = get_peft_model(model, LoraConfig(r=args.rank, lora_alpha=args.rank * 2,
        lora_dropout=0.05, target_modules=["q_proj", "v_proj"], bias="none", task_type="CAUSAL_LM"))
    model.print_trainable_parameters()

    def collate(records):
        length = max(len(row["input_ids"]) for row in records)
        result = {}
        for field in ("input_ids", "attention_mask", "labels"):
            pad = -100 if field == "labels" else tokenizer.pad_token_id if field == "input_ids" else 0
            result[field] = torch.tensor([row[field] + [pad] * (length - len(row[field])) for row in records])
        return result

    args.output.mkdir(parents=True, mode=0o700)
    trainer = Trainer(model=model, args=TrainingArguments(output_dir=str(args.output / "checkpoints"),
        max_steps=args.steps, per_device_train_batch_size=1, per_device_eval_batch_size=1,
        gradient_accumulation_steps=1, learning_rate=1e-4, bf16=True, gradient_checkpointing=True,
        optim="adamw_torch", save_strategy="no", logging_steps=1, report_to="none", seed=args.seed,
        dataloader_num_workers=0), train_dataset=train_tokens, eval_dataset=validation_tokens, data_collator=collate)
    torch.cuda.empty_cache()
    baseline = trainer.evaluate()["eval_loss"]
    trainer.train()
    trained = trainer.evaluate()["eval_loss"]
    model.save_pretrained(args.output, safe_serialization=True)
    tokenizer.save_pretrained(args.output)
    report = {**summary, "steps": args.steps, "rank": args.rank, "max_length": args.max_length,
        "validation_loss_before": baseline, "validation_loss_after": trained,
        "held_out_loss_improved": math.isfinite(trained) and trained < baseline,
        "seconds": time.monotonic() - started, "peak_gpu_bytes": torch.cuda.max_memory_allocated(),
        "activated": False, "quality_verified": False,
        "warning": "Loss is not chat quality. Review held-out replies and tool behavior before activating. Deleting examples does not untrain this adapter."}
    (args.output / "evaluation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, ImportError) as error:
        print(f"No adapter activated: {error}", file=sys.stderr)
        raise SystemExit(1)
