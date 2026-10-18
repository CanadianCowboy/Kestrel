#!/usr/bin/env python3
"""Train a local Qwen3 Base behavior adapter from reviewed or consented chat JSONL.

No uploads, remote code, or direct adapter activation. The desktop may invoke
bounded training after one-time chat-learning consent; standalone runs still
require --approve-training. All weights and training tools must be installed
locally. The packaged astronomy seed corpus is merged only with --include-seeds
so a manual generic run is unchanged; see docs/ASTRONOMY.md.
"""
from __future__ import annotations

import argparse
import hashlib
from difflib import SequenceMatcher
import json
import math
import os
from pathlib import Path
import random
import re
import sys
import time

BASE_MODEL = "Qwen/Qwen3-4B-Base"
INSTRUCT_MODEL = "Qwen/Qwen3-4B-Instruct-2507"
PARENT_MODELS = (BASE_MODEL, INSTRUCT_MODEL)
ADAPTER_STAGES = ("persona", "tool")

# The field Kestrel is tuned for. An example may declare `domain`; when it does
# not, the text is classified by a small lexicon so an ordinary learning export
# takes part in domain curation without new metadata or a model call. This is a
# routing signal, not a correctness claim about the example.
DOMAINS = ("general", "astronomy")
DOMAIN_ALIASES = {"astronomy": "astronomy", "astrophysics": "astronomy",
                  "astro": "astronomy", "general": "general"}
MIN_DOMAIN_PROMPTS = 4
SEED_DATASET = Path(__file__).with_name("astronomy_seed.jsonl")
TOOL_SEED_DATASET = Path(__file__).with_name("tool_seed.jsonl")

_ASTRO_TERMS = (
    "astronomy", "astronomical", "astrophysics", "astrophysical",
    "telescope", "telescopes", "observatory", "galaxy", "galaxies", "galactic",
    "nebula", "nebulae", "supernova", "supernovae", "exoplanet", "exoplanets",
    "planet", "planets", "planetary", "orbital", "orbit", "orbits", "comet",
    "comets", "asteroid", "asteroids", "meteor", "meteors", "meteorite",
    "quasar", "quasars", "pulsar", "pulsars", "magnetar", "cosmology",
    "cosmological", "redshift", "parallax", "parsec", "parsecs", "spectroscopy",
    "spectroscopic", "spectrum", "spectra", "photometry", "photometric",
    "occultation", "transit", "transits", "accretion", "protostar",
    "protoplanetary", "heliosphere", "photosphere", "chromosphere", "corona",
    "isochrone", "cepheid", "interstellar", "stellar", "solar",
    "lunar", "sun", "moon", "solar system", "milky way", "andromeda",
    "eclipse", "eclipses", "solstice", "equinox", "magnitude",
    "luminosity", "bolometric", "hubble", "jwst", "webb", "kepler", "tess",
    "gaia", "chandra", "alma", "star", "stars", "black hole", "black holes",
    "neutron star", "neutron stars", "white dwarf", "white dwarfs", "red giant",
    "brown dwarf", "brown dwarfs", "main-sequence", "hertzsprung", "light-year",
    "light-years", "dark matter", "dark energy", "big bang",
    "cosmic microwave", "cmb", "habitable zone",
)
# Case-insensitive whole-word matching, so a substring such as "start" is not
# tagged as astronomy. Multi-word entries are matched as written.
_ASTRO_PATTERN = re.compile(
    r"\b(?:" + "|".join(re.escape(term) for term in _ASTRO_TERMS) + r")\b",
    re.IGNORECASE)


def normalize_domain(value) -> str:
    if not isinstance(value, str):
        raise ValueError('The domain field must be a string, for example "astronomy".')
    domain = DOMAIN_ALIASES.get(value.strip().casefold())
    if domain is None:
        raise ValueError(f"Unknown domain {value!r}; use one of {', '.join(DOMAINS)}.")
    return domain


def infer_domain(record: dict) -> str:
    text = f"{record.get('prompt', '')}\n{record.get('preferred', '')}"
    return "astronomy" if _ASTRO_PATTERN.search(text) else "general"


def domain_for_record(record: dict) -> str:
    declared = record.get("domain")
    if declared is None or (isinstance(declared, str) and not declared.strip()):
        return infer_domain(record)
    return normalize_domain(declared)


def domain_prompt_count(examples: list[dict], domain: str) -> int:
    domain = normalize_domain(domain)
    return len({prompt_key(item["prompt"]) for item in examples if item.get("domain") == domain})


def domain_counts(examples: list[dict]) -> dict:
    counts = {name: 0 for name in DOMAINS}
    for item in examples:
        domain = item.get("domain", "general")
        counts[domain] = counts.get(domain, 0) + 1
    return counts


def require_domain(examples: list[dict], domain: str, minimum: int = MIN_DOMAIN_PROMPTS) -> None:
    """Refuse a specialist run whose holdout would contain no specialist prompts."""
    domain = normalize_domain(domain)
    if domain == "general":
        return
    available = domain_prompt_count(examples, domain)
    if available < minimum:
        raise ValueError(
            f"Only {available} distinct {domain} prompts are available; {minimum} are needed so the "
            f"held-out check covers the field. Add {minimum - available} more, or pass "
            "--include-seeds to use the packaged reviewed astronomy examples.")


def merge_examples(examples: list[dict], seeds: list[dict]) -> tuple[list[dict], int]:
    """Add packaged reviewed seeds without overriding the user's own correction.

    A prompt the user has already taught keeps their answer; the seed only fills
    a prompt the dataset does not cover at all. That way packaged material can
    keep the field from disappearing out of the adapter, but it can never win an
    argument against an explicit correction.
    """
    prompts = {prompt_key(item["prompt"]) for item in examples}
    merged = list(examples)
    added = 0
    for seed in seeds:
        key = prompt_key(seed["prompt"])
        if key in prompts:
            continue
        merged.append(seed)
        prompts.add(key)
        added += 1
    if len(merged) > 1000:
        raise ValueError("Seeds plus the dataset exceed 1000 examples; remove examples before training.")
    if len(prompts) < 2:
        raise ValueError("Use at least two distinct prompts; seeds alone cannot replace a dataset.")
    return merged, added


def load_examples(path: Path, allow_consented_chat: bool = False,
                  allow_imported: bool = False, base_model: str = BASE_MODEL,
                  adapter_stage: str = "persona", select_stage: bool = False) -> list[dict]:
    if not path.is_file():
        raise ValueError(f"Dataset not found: {path}. Pass a JSONL file the trainer can read.")
    if path.stat().st_size > 16 * 1024 * 1024:
        raise ValueError("Dataset exceeds 16 MiB; export at most 1000 reviewed examples.")
    examples = []
    seen = set()
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        record = json.loads(line)
        if not isinstance(record, dict) or record.get("schema_version") != 1:
            raise ValueError(f"Line {line_number}: expected a schema_version 1 example.")
        # Imported material is authorized by the run flag rather than by a
        # review flag, and its provenance block below says so explicitly. Every
        # other source still needs a user review or opted-in chat consent.
        imported = record.get('source') == 'imported'
        if (not imported and record.get("reviewed") is not True
                and not (allow_consented_chat and record.get("chat_learning_consent") is True)):
            raise ValueError(f"Line {line_number}: explicit review or opted-in chat-learning authorization is required.")
        parent = record.get("base_model")
        stage = record.get("adapter_stage", "persona")
        if parent not in PARENT_MODELS or stage not in ADAPTER_STAGES:
            raise ValueError(f"Line {line_number}: unknown parent model or adapter stage.")
        if select_stage and (parent != base_model or stage != adapter_stage):
            continue
        if parent != base_model or stage != adapter_stage:
            raise ValueError(f"Line {line_number}: dataset must target {base_model} / {adapter_stage}.")
        if record.get('source') == 'tool_trace' and (stage != 'tool' or record.get('tool_success') is not True):
            raise ValueError(f"Line {line_number}: tool traces require tool-stage successful execution provenance.")
        if stage == 'tool':
            parse_tool_target(record.get('preferred', ''))
        for field in ("prompt", "preferred", "rejected"):
            text = record.get(field, "")
            if not isinstance(text, str) or len(text.encode("utf-8")) > 4000 or (field != "rejected" and not text.strip()):
                raise ValueError(f"Line {line_number}: invalid {field}; use nonempty text up to 4000 UTF-8 bytes.")
        source = record.get('source', 'reviewed' if record.get('reviewed') is True else 'chat')
        if source not in ('reviewed', 'teaching', 'chat', 'seed', 'imported', 'tool_trace'):
            raise ValueError(f"Line {line_number}: unknown training example provenance.")
        if source in ('reviewed', 'seed') and record.get('reviewed') is not True:
            raise ValueError(f"Line {line_number}: {source} provenance requires explicit review.")
        if source == 'imported':
            # Imported text is not reviewed, and it cannot be trained on without
            # the caller saying so explicitly. Its licence and origin travel
            # with every example so a report can say what the weights learned
            # from and under what terms.
            if not allow_imported:
                raise ValueError(
                    f"Line {line_number}: imported examples need an explicit --allow-imported run.")
            if record.get('reviewed') is True:
                raise ValueError(f"Line {line_number}: imported examples cannot claim user review.")
            for field in ('license', 'origin'):
                if not isinstance(record.get(field), str) or not record.get(field, '').strip():
                    raise ValueError(f"Line {line_number}: imported examples need a nonempty {field}.")
        try:
            record['domain'] = domain_for_record(record)
        except ValueError as error:
            raise ValueError(f"Line {line_number}: {error}") from error
        key = (prompt_key(record["prompt"]), record["preferred"].strip())
        if key in seen:
            raise ValueError(f"Line {line_number}: duplicate prompt/preferred pair.")
        seen.add(key)
        examples.append(record)
    if not 2 <= len(examples) <= 1000:
        raise ValueError("Use 2–1000 distinct reviewed examples; at least one must be held out for evaluation.")
    # Identical prompts must never occur on both sides of the split.
    if len({prompt_key(entry["prompt"]) for entry in examples}) < 2:
        raise ValueError("Use at least two distinct prompts to keep validation separate from training.")
    return examples


def prompt_key(text: str) -> str:
    return ' '.join(text.casefold().split())


def curate_examples(examples: list[dict], limit: int = 128, chat_limit: int = 32,
                    domain: str | None = None) -> tuple[list[dict], dict]:
    # Explicit user targets outrank self-distilled replies and packaged review
    # material; the newest explicit correction wins whether taught in chat or
    # reviewed in the panel.
    if limit < 2 or chat_limit < 0:
        raise ValueError('Use a replay limit of at least two and a nonnegative chat limit.')
    chosen = {}
    priority = {'chat': 0, 'tool_trace': 0, 'seed': 1, 'imported': 1, 'reviewed': 2, 'teaching': 2}
    for record in examples:
        source = record.get('source', 'reviewed' if record.get('reviewed') is True else 'chat')
        if source not in priority:
            raise ValueError('Unknown training example provenance.')
        item = {**record, 'source': source, 'domain': domain_for_record(record)}
        key = prompt_key(item['prompt'])
        if key not in chosen or priority[source] >= priority[chosen[key]['source']]:
            chosen.pop(key, None)  # insertion order now tracks the winning revision
            chosen[key] = item
    explicit = [item for item in chosen.values() if item['source'] not in ('chat', 'tool_trace')]
    chat = [item for item in chosen.values() if item['source'] in ('chat', 'tool_trace')]
    # Recent chat is a capped replay buffer, never a reason to drown out a
    # correction with thousands of copies of the model's own prior behavior.
    selected = explicit[-limit:]
    replay_count = min(chat_limit, max(0, limit - len(selected)))
    if replay_count:
        selected += chat[-replay_count:]
    report = {'input': len(examples), 'selected': len(selected),
              'conflicting_or_duplicate_prompts': len(examples) - len(chosen),
              'chat_selected': sum(item['source'] in ('chat', 'tool_trace') for item in selected),
              'explicit_selected': sum(item['source'] not in ('chat', 'tool_trace') for item in selected),
              'imported_selected': sum(item['source'] == 'imported' for item in selected)}
    if domain is not None:
        domain = normalize_domain(domain)
        reserved = 0
        if domain != 'general':
            selected, reserved = reserve_domain_slots(selected, chosen, domain, limit)
        report.update({'domain': domain, 'domain_selected': sum(item['domain'] == domain for item in selected),
                       'domain_reserved': reserved})
    return selected, report


def reserve_domain_slots(selected: list[dict], chosen: dict, domain: str,
                         limit: int) -> tuple[list[dict], int]:
    """Keep a share of the replay budget for the specialist field.

    Curation otherwise keeps only the newest explicit corrections, so a user who
    corrects generic chat for a week would silently train the field out of the
    adapter. The newest specialist material replaces the oldest non-specialist
    material, never the other way round, and the result stays in source order so
    the report and the split remain deterministic.
    """
    order = {prompt_key(item['prompt']): index for index, item in enumerate(chosen.values())}
    wanted = max(2, limit // 2)
    present = sum(item['domain'] == domain for item in selected)
    selected_keys = {prompt_key(item['prompt']) for item in selected}
    pool = [item for item in chosen.values()
            if item['domain'] == domain and prompt_key(item['prompt']) not in selected_keys]
    needed = min(max(0, wanted - present), len(pool))
    if needed == 0:
        return selected, 0
    drop = {prompt_key(item['prompt'])
            for item in [entry for entry in selected if entry['domain'] != domain][:needed]}
    kept = [item for item in selected if prompt_key(item['prompt']) not in drop]
    kept += pool[-needed:]
    kept.sort(key=lambda item: order[prompt_key(item['prompt'])])
    return kept, needed


def split_examples(examples: list[dict], seed: int = 42) -> tuple[list[dict], list[dict]]:
    prompts = sorted({prompt_key(entry["prompt"]) for entry in examples})
    if len(prompts) < 2:
        raise ValueError('Use at least two distinct normalized prompts for a disjoint holdout.')
    random.Random(seed).shuffle(prompts)
    count = max(1, math.ceil(len(prompts) * 0.2))
    held_out = set(prompts[:count])
    return ([entry for entry in examples if prompt_key(entry["prompt"]) not in held_out],
            [entry for entry in examples if prompt_key(entry["prompt"]) in held_out])


def parse_tool_target(text: str) -> dict:
    if not isinstance(text, str) or not text.startswith('<tool_call>') or not text.endswith('</tool_call>'):
        raise ValueError('Tool targets must contain exactly one tagged JSON call.')
    payload = text[len('<tool_call>'):-len('</tool_call>')]
    try:
        call = json.loads(payload)
    except json.JSONDecodeError:
        # JSON string values inside tool arguments may include literal markup,
        # which must not be mistaken for a second call terminator.
        end = text.rfind('</tool_call>')
        start = text.find('<tool_call>') + len('<tool_call>')
        call = json.loads(text[start:end])
    if not isinstance(call, dict) or not isinstance(call.get('name'), str) or not call['name'].strip() or not isinstance(call.get('arguments'), dict):
        raise ValueError('Tool targets need a tool name and an arguments object.')
    return call


def training_prefix(tokenizer, example: dict) -> str:
    if example.get('base_model', BASE_MODEL) == INSTRUCT_MODEL:
        return tokenizer.apply_chat_template([{'role': 'user', 'content': example['prompt'].strip()}],
            tokenize=False, add_generation_prompt=True)
    return f"user: {example['prompt'].strip()}\nassistant:"


def encode_example(tokenizer, example: dict, max_length: int) -> dict:
    # Match the exact parent's prompting, and learn only the assistant suffix.
    prefix = training_prefix(tokenizer, example)
    prompt_ids = tokenizer.encode(prefix, add_special_tokens=False)
    answer_ids = tokenizer.encode(" " + example["preferred"].strip(), add_special_tokens=False)
    input_ids = prompt_ids + answer_ids + [tokenizer.eos_token_id]
    if len(input_ids) > max_length:
        raise ValueError("An example exceeds --max-length; shorten it or explicitly increase the limit. No text was truncated.")
    return {"input_ids": input_ids, "attention_mask": [1] * len(input_ids),
            "labels": [-100] * len(prompt_ids) + answer_ids + [tokenizer.eos_token_id]}


def verify_local_lineage(training_model: Path, inference_model: Path | None, base_model: str = BASE_MODEL) -> None:
    if inference_model is None:
        raise ValueError("Automatic training requires the selected local inference model's manifest.")
    inference = json.loads(inference_model.with_suffix('.manifest.json').read_text())
    if inference.get('base_model') != base_model or inference.get('filename') != inference_model.name:
        raise ValueError("Inference manifest does not identify the selected exact parent model lineage.")
    def verify(path: Path, record: dict) -> None:
        digest = hashlib.sha256()
        with path.open('rb') as source:
            for block in iter(lambda: source.read(4 * 1024 * 1024), b''):
                digest.update(block)
        if path.stat().st_size != record.get('bytes') or digest.hexdigest() != record.get('sha256'):
            raise ValueError(f"Local weight checksum mismatch: {path.name}. Reinstall verified weights.")
    verify(inference_model, inference)
    training = json.loads((training_model / 'download-manifest.json').read_text())
    expected_repository = 'unsloth/Qwen3-4B-Base-bnb-4bit' if base_model == BASE_MODEL else 'unsloth/Qwen3-4B-Instruct-2507-bnb-4bit'
    if training.get('repository') != expected_repository:
        raise ValueError("Training manifest does not identify the selected exact parent model lineage.")
    files = {item['filename']: item for item in training.get('files', [])}
    for name in ('model.safetensors', 'config.json', 'tokenizer.json', 'tokenizer_config.json'):
        verify(training_model / name, files[name])


def activation_eligible(before: float, after: float, behavior_before: dict | None,
                        behavior_after: dict | None, domain: str | None = None) -> bool:
    if not (math.isfinite(before) and math.isfinite(after) and after < before
            and behavior_before is not None and behavior_after is not None):
        return False
    scores = (behavior_before.get('score'), behavior_after.get('score'))
    if not (all(isinstance(score, (int, float)) and math.isfinite(score) and 0 <= score <= 1
                for score in scores)
            and behavior_after.get('safe_outputs') is True
            and scores[1] >= scores[0]):
        return False
    # An aggregate improvement can hide a regression on the one field the
    # adapter exists for, so when a domain was requested its own held-out score
    # must not fall. This is still a similarity probe, not a quality proof.
    if domain is not None and normalize_domain(domain) != 'general':
        field = normalize_domain(domain)
        field_scores = ((behavior_before.get('domain_scores') or {}).get(field),
                        (behavior_after.get('domain_scores') or {}).get(field))
        if not all(isinstance(score, (int, float)) and math.isfinite(score) and 0 <= score <= 1
                   for score in field_scores):
            return False
        return field_scores[1] >= field_scores[0]
    return True


def behavior_probe(model, tokenizer, examples: list[dict]) -> dict:
    # Held-out prompts were never in the training split. This catches obvious
    # regressions/empty/tool-call output, but is deliberately not a quality claim.
    import torch
    scores = []
    probes = []
    by_domain = {}
    safe = True
    model.eval()
    with torch.inference_mode(), torch.autocast(device_type='cuda', dtype=torch.bfloat16):
        for example in examples[:4]:
            encoded = tokenizer(training_prefix(tokenizer, example),
                                return_tensors="pt", add_special_tokens=False).to(model.device)
            generated = model.generate(**encoded, max_new_tokens=192 if example.get('adapter_stage') == 'tool' else 48, do_sample=False,
                                       pad_token_id=tokenizer.eos_token_id, use_cache=False)
            text = tokenizer.decode(generated[0, encoded['input_ids'].shape[1]:], skip_special_tokens=True).strip()
            # Match the base-model runtime's next-speaker boundary filter.
            for marker in ('\nuser:', '\nassistant:', '\nsystem:'):
                text = text.split(marker, 1)[0].strip()
            if example.get('adapter_stage', 'persona') == 'tool':
                try:
                    actual = parse_tool_target(text)
                    score = float(actual == parse_tool_target(example['preferred']))
                except (ValueError, TypeError):
                    score = 0.0
                    safe = False
            else:
                safe = safe and bool(text) and not any(tag in text.lower() for tag in ('<tool_call>', 'user:', 'system:', 'nan'))
                score = SequenceMatcher(None, text.casefold(), example['preferred'].strip().casefold()).ratio()
            scores.append(score)
            field = example.get('domain', 'general')
            by_domain.setdefault(field, []).append(score)
            # The report is inspectable without retaining prompts or generated
            # replies outside the encrypted profile.
            probes.append({'prompt_sha256': hashlib.sha256(prompt_key(example['prompt']).encode()).hexdigest(),
                           'source': example.get('source', 'reviewed'), 'domain': field, 'score': score})
    return {'score': sum(scores) / len(scores), 'safe_outputs': safe, 'prompts': len(scores),
            'domain_scores': {field: sum(values) / len(values) for field, values in by_domain.items()},
            'probes': probes}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--model", type=Path, help="Local exact-parent Qwen3 4B HF directory; not a GGUF")
    parser.add_argument("--base-model", choices=PARENT_MODELS, default=BASE_MODEL)
    parser.add_argument("--adapter-stage", choices=ADAPTER_STAGES, default="persona")
    parser.add_argument("--select-stage", action="store_true", help="Select only this parent/stage from a mixed local export")
    parser.add_argument("--output", type=Path, help="New adapter directory (must not exist)")
    parser.add_argument("--validate-only", action="store_true", help="Validate and summarize without importing training libraries")
    parser.add_argument("--steps", type=int, default=8)
    parser.add_argument("--max-length", type=int, default=128)
    parser.add_argument("--rank", type=int, default=4)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--approve-training", action="store_true", help="Explicitly authorize this GPU training run")
    parser.add_argument("--allow-consented-chat", action="store_true", help="Accept examples explicitly marked chat_learning_consent")
    parser.add_argument("--allow-imported", action="store_true", help="Accept openly licensed dataset examples marked source=imported")
    parser.add_argument("--automatic-evaluation", action="store_true", help="Apply bounded held-out generation and loss gates for the desktop")
    parser.add_argument("--inference-model", type=Path, help="Verify this local inference GGUF against its adjacent pinned manifest")
    parser.add_argument("--domain", choices=DOMAINS, help="Field this adapter targets; astronomy requires a domain-held-out check")
    parser.add_argument("--include-seeds", action="store_true", help="Merge the packaged reviewed astronomy seed examples")
    parser.add_argument("--include-tool-seeds", action="store_true", help="Merge reviewed protocol seeds into Base/Tool training")
    args = parser.parse_args()
    examples = load_examples(args.dataset, args.allow_consented_chat, args.allow_imported,
                             args.base_model, args.adapter_stage, args.select_stage)
    seeds_added = 0
    if args.include_seeds and (args.base_model != BASE_MODEL or args.adapter_stage != 'persona'):
        raise ValueError('Astronomy persona seeds target Base/Persona only; do not mix them into another adapter.')
    if args.include_seeds:
        examples, seeds_added = merge_examples(examples, load_examples(SEED_DATASET))
    if args.include_tool_seeds:
        if args.base_model != BASE_MODEL or args.adapter_stage != 'tool':
            raise ValueError('Tool seeds target Base/Tool only.')
        examples, seeds_added = merge_examples(examples, load_examples(TOOL_SEED_DATASET, adapter_stage='tool'))
    curation = None
    if args.automatic_evaluation:
        examples, curation = curate_examples(examples, domain=args.domain)
    if args.domain is not None:
        require_domain(examples, args.domain)
    train, validation = split_examples(examples, args.seed)
    summary = {"examples": len(examples), "train": len(train), "validation": len(validation),
               "dataset_sha256": hashlib.sha256(args.dataset.read_bytes()).hexdigest(),
               "base_model": args.base_model, "adapter_stage": args.adapter_stage, "curation": curation, "seed": args.seed,
               "domain": args.domain, "seeds_added": seeds_added,
               "domain_counts": domain_counts(examples),
               "domain_prompts": {name: domain_prompt_count(examples, name) for name in DOMAINS}}
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
    if args.automatic_evaluation:
        verify_local_lineage(args.model, args.inference_model, args.base_model)
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
    skipped = 0
    if args.automatic_evaluation:
        eligible = []
        for item in examples:
            try:
                encode_example(tokenizer, item, args.max_length)
                eligible.append(item)
            except ValueError:
                skipped += 1
        if len({prompt_key(item['prompt']) for item in eligible}) < 8:
            raise ValueError("Not enough distinct examples fit the local token budget (8 needed). Long examples were skipped, not truncated.")
        examples = eligible
        train, validation = split_examples(examples, args.seed)
    train_tokens = [encode_example(tokenizer, item, args.max_length)
                    for item in train
                    for _ in range(2 if args.automatic_evaluation and item.get('source') in ('reviewed', 'teaching') else 1)]
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
    baseline_behavior = behavior_probe(model, tokenizer, validation) if args.automatic_evaluation else None
    trainer.train()
    trained = trainer.evaluate()["eval_loss"]
    trained_behavior = behavior_probe(model, tokenizer, validation) if args.automatic_evaluation else None
    model.save_pretrained(args.output, safe_serialization=True)
    tokenizer.save_pretrained(args.output)
    report = {**summary, "steps": args.steps, "rank": args.rank, "max_length": args.max_length,
        "validation_loss_before": baseline, "validation_loss_after": trained,
        "held_out_loss_improved": math.isfinite(trained) and trained < baseline,
        "seconds": time.monotonic() - started, "peak_gpu_bytes": torch.cuda.max_memory_allocated(),
        "activated": False, "quality_verified": False,
        "inference_sha256": json.loads(args.inference_model.with_suffix('.manifest.json').read_text())['sha256'] if args.inference_model else None,
        "skipped_overlength_examples": skipped, "curation": curation,
        "examples": len(examples), "train": len(train), "validation": len(validation),
        "train_prompts": len(train), "held_out_prompts": len(validation),
        "weighted_train_examples": len(train_tokens),
        "held_out_prompt_sha256": [hashlib.sha256(prompt_key(item['prompt']).encode()).hexdigest()
                                  for item in validation],
        "behavior_before": baseline_behavior, "behavior_after": trained_behavior,
        "automatic_activation_eligible": activation_eligible(baseline, trained, baseline_behavior,
                                                             trained_behavior, args.domain),
        "seeds_added": seeds_added, "domain_counts": domain_counts(examples),
        "domain_prompts": {name: domain_prompt_count(examples, name) for name in DOMAINS},
        "warning": "Automated loss and behavior probes are limited, not proof of general quality. Reset/rollback remains available. Deleting examples does not untrain this adapter."}
    (args.output / "evaluation.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError, ImportError, KeyError) as error:
        print(f"No adapter activated: {error}", file=sys.stderr)
        raise SystemExit(1)
