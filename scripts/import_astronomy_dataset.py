#!/usr/bin/env python3
"""Convert a locally downloaded astronomy question/answer dataset to Kestrel JSONL.

Nothing here downloads anything. Materialize a Hugging Face dataset yourself
(for example with `datasets.load_dataset(...).to_json("astro.jsonl")`), then
point this script at the file. The output is schema_version 1 with
`source: "imported"`, `reviewed: false`, and the licence and origin recorded, so
the trainer can require an explicit `--allow-imported` run and the evaluation
report never implies that imported text was reviewed by the user.

See docs/ASTRONOMY.md for the datasets this was written against and their
licensing caveats.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile

BASE_MODEL = "Qwen/Qwen3-4B-Base"
MAX_FIELD_BYTES = 4000
MAX_EXAMPLES = 1000


def prompt_key(text: str) -> str:
    return ' '.join(text.casefold().split())


def read_records(path: Path) -> list[dict]:
    if path.stat().st_size > 256 * 1024 * 1024:
        raise ValueError("Input exceeds 256 MiB; export only the split you intend to import.")
    records = []
    for line_number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        if not line.strip():
            continue
        try:
            record = json.loads(line)
        except json.JSONDecodeError as error:
            raise ValueError(f"Line {line_number} is not valid JSON: {error.msg}.") from error
        if not isinstance(record, dict):
            raise ValueError(f"Line {line_number} must be a JSON object.")
        records.append(record)
    if not records:
        raise ValueError("The input file contains no records.")
    return records


def convert(records: list[dict], prompt_field: str, answer_field: str, domain: str,
            license_name: str, origin: str, limit: int = MAX_EXAMPLES,
            min_answer_chars: int = 40) -> tuple[list[dict], dict]:
    """Map question/answer columns onto Kestrel's schema, refusing bad rows.

    The first answer for a normalized prompt wins, because a dataset that asks
    the same question twice usually disagrees with itself and averaging the two
    is not something a small adapter can do coherently.
    """
    if not 1 <= limit <= MAX_EXAMPLES:
        raise ValueError(f"Use a limit between 1 and {MAX_EXAMPLES}; the trainer refuses more.")
    if min_answer_chars < 0:
        raise ValueError("The minimum answer length cannot be negative.")
    if not license_name.strip() or not origin.strip():
        raise ValueError("Record the dataset licence and origin; an imported example without them is not traceable.")
    selected = []
    seen = set()
    skipped = {'missing_field': 0, 'too_short': 0, 'too_long': 0, 'duplicate_prompt': 0}
    for record in records:
        prompt = record.get(prompt_field)
        answer = record.get(answer_field)
        if not isinstance(prompt, str) or not isinstance(answer, str) or not prompt.strip() or not answer.strip():
            skipped['missing_field'] += 1
            continue
        prompt = prompt.strip()
        answer = answer.strip()
        if len(answer) < min_answer_chars:
            skipped['too_short'] += 1
            continue
        if (len(prompt.encode('utf-8')) > MAX_FIELD_BYTES
                or len(answer.encode('utf-8')) > MAX_FIELD_BYTES):
            skipped['too_long'] += 1
            continue
        key = prompt_key(prompt)
        if key in seen:
            skipped['duplicate_prompt'] += 1
            continue
        seen.add(key)
        selected.append({
            'schema_version': 1,
            'reviewed': False,
            'base_model': BASE_MODEL,
            'domain': domain,
            'source': 'imported',
            'license': license_name.strip(),
            'origin': origin.strip(),
            'prompt': prompt,
            'preferred': answer,
            'rejected': '',
        })
        if len(selected) >= limit:
            break
    if len(selected) < 2:
        raise ValueError("Fewer than two usable pairs survived; adjust the field names or the filters.")
    report = {'input': len(records), 'selected': len(selected), 'skipped': skipped,
              'domain': domain, 'license': license_name.strip(), 'origin': origin.strip()}
    return selected, report


def write_dataset(path: Path, records: list[dict]) -> None:
    if path.exists():
        raise ValueError("Output already exists. Choose a new file; imports are never overwritten.")
    payload = ''.join(json.dumps(record, ensure_ascii=False) + '\n' for record in records)
    path.parent.mkdir(parents=True, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(dir=path.parent, prefix='.import-', suffix='.jsonl')
    try:
        with os.fdopen(descriptor, 'w', encoding='utf-8') as handle:
            handle.write(payload)
        if hasattr(os, 'chmod'):
            os.chmod(temporary, 0o600)
        os.replace(temporary, path)
    except BaseException:
        Path(temporary).unlink(missing_ok=True)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--input', type=Path, required=True, help='Local JSONL file to convert')
    parser.add_argument('--output', type=Path, required=True, help='New JSONL file to write')
    parser.add_argument('--prompt-field', default='question')
    parser.add_argument('--answer-field', default='answer')
    parser.add_argument('--domain', default='astronomy', choices=('astronomy', 'general'))
    parser.add_argument('--license', required=True, help='Licence as stated by the dataset, for example "mit"')
    parser.add_argument('--origin', required=True, help='Dataset identifier or URL, for example UniverseTBD/arxiv-qa-astro-ph')
    parser.add_argument('--limit', type=int, default=MAX_EXAMPLES)
    parser.add_argument('--min-answer-chars', type=int, default=40)
    args = parser.parse_args()
    records = read_records(args.input)
    selected, report = convert(records, args.prompt_field, args.answer_field, args.domain,
                               args.license, args.origin, args.limit, args.min_answer_chars)
    write_dataset(args.output, selected)
    report['output'] = str(args.output)
    report['dataset_sha256'] = hashlib.sha256(args.output.read_bytes()).hexdigest()
    print(json.dumps(report, indent=2))
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (ValueError, OSError, KeyError) as error:
        print(f"No dataset imported: {error}", file=sys.stderr)
        raise SystemExit(1)
