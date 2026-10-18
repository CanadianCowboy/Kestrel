# The astronomy specialism

Kestrel is tuned for astronomy and astrophysics. That is one decision expressed
in three places, and they are meant to agree:

1. **The prompt.** `core::Persona` defaults to `PersonaSpecialism::Astronomy`,
   and `Persona::specialismLine()` adds the domain paragraph to the shared
   system prompt. It is built from the specialism alone, never from the drifting
   dials, so it stays byte-identical and the backend's cached prefix survives.
   Switching the persona to `General` removes the paragraph rather than leaving
   a generalist told to act like a specialist.
2. **The training target.** The local LoRA pipeline is told `--domain astronomy`
   and keeps packaged, reviewed astronomy examples in every run.
3. **The tools.** Kestrel has local, permission-free astronomy arithmetic in
   `src/core/astronomy.*`, exposed to the model and to a local MCP client as
   four tools.

## What the prompt can and cannot do

The paragraph steers attention and habits of answer: state units, epochs,
frames, and bands; keep measurements separate from model-dependent inference;
quote uncertainties when they change the answer; do not invent measurements or
citations. It cannot add knowledge to a small local model. A prompt that says
"be precise about cosmology" still produces a model that reasons within its
weights, and the trained adapter is what carries the field's content.

## Tools

All four are pure computation: no network, no filesystem, no catalogue, no
clock. They declare no permission, so they are enabled immediately and appear
in the model's capability block. They are also visible to a local MCP client
when the endpoint runs, which is why they are described in the prompt as local
computations rather than as external tools.

| Tool | Arguments | Returns |
| --- | --- | --- |
| `astronomy_time` | `date` | JD and MJD for an ISO 8601 UTC instant, with the scale caveat |
| `astronomy_coordinates` | `ra`, `dec`, optional `other_ra`, `other_dec` | decimal and sexagesimal values, plus great-circle separation |
| `astronomy_length` | `value`, `from`, `to` | conversion between m, km, au, ly, pc, kpc, mpc, rsun, rearth |
| `astronomy_magnitudes` | `magnitude_difference`, or `apparent_magnitude` + `absolute_magnitude` | flux ratio, or distance from the distance modulus |

Examples:

```text
astronomy_time {"date": "2026-10-17T21:04:00Z"}
  JD 2461331.3777778, MJD 61330.877777778. That is the UTC scale; TT runs about
  69 s ahead in 2026, ...

astronomy_coordinates {"ra": "05:34:31.94", "dec": "+22:00:52.2"}
  RA 83.6331 deg (05h34m31.94s), Dec 22.0145 deg (+22d00m52.20s). No reference
  frame is implied: ...

astronomy_magnitudes {"apparent_magnitude": 10, "absolute_magnitude": 5}
  m - M = 5 gives 100 pc, ignoring absorption. ...
```

Refusals are deliberate and actionable: a date that does not exist, a
right ascension of 25 hours, an unknown unit, or a sexagesimal value with 61
minutes is answered with what to pass instead. `astronomy_time` accepts only
UTC (`Z` or no suffix); an offset such as `+02:00` is refused rather than
silently reinterpreted.

## Training pipeline

`scripts/train_persona.py` gained five things for the specialism. Each one is
covered by `tests/train_persona_tests.py`, which CTest runs as
`kestrel_training_data_tests`.

### Domain inference

Every example carries `domain`. A dataset may declare it; when the field is
absent, the loader classifies the prompt and preferred answer with a small
keyword lexicon (`infer_domain`). That is what makes an existing learning export
from the app take part in domain curation without a profile migration. An
unknown declared domain is an error, not a silent `general`.

### Packaged seed corpus

`scripts/astronomy_seed.jsonl` contains 24 reviewed astronomy examples
(`source: "seed"`, `domain: "astronomy"`) covering magnitudes, parallaxes,
redshift, spectral classes, solar constants, time scales, escape velocity,
frames, novae and supernovae, resolution, luminosity, mass–luminosity, the CMB,
equilibrium temperature, distance modulus, RA/Dec, AB magnitudes, cluster ages,
comets, Kepler's third law, Olbers' paradox, and uncertainty reporting.

`--include-seeds` merges them. The merge never overrides the user's own answer
for the same prompt: a taught correction always wins. The desktop's automatic
learning path passes `--include-seeds --domain astronomy` on every run, so a
week of ordinary chat cannot train the field out of the adapter, and the
domain-held-out check always has material.

### Domain-balanced curation

Curation used to keep only the newest explicit corrections, which means a long
run of generic corrections could crowd out the field entirely. When a domain is
requested, `reserve_domain_slots` keeps at least half of the replay budget for
that domain (minimum two examples), replacing the *oldest* non-specialist picks
with the *newest* specialist ones. The result stays in source order, so the
split and the report are deterministic.

### Domain-held-out activation gate

The candidate must still improve held-out loss, produce safe outputs, and not
regress on the aggregate behaviour probe. With `--domain astronomy` it must also
not regress on the astronomy-only probe score. An aggregate improvement that
trades away the field is rejected. This is still a text-similarity probe, not a
quality claim: it catches a regression, it does not prove correctness.

### Imported provenance

`--allow-imported` accepts examples marked `source: "imported"`. They must not
claim `reviewed: true` and must carry a non-empty `license` and `origin`, so the
evaluation report can say what was learned from and under what terms. Imported
material ranks below a user correction in curation. The app never sets this
flag, and never downloads a dataset.

## Hugging Face datasets

Checked against the Hugging Face API on 2026-10-17. Licences are quoted as the
dataset card declares them; **verify terms yourself before training on any of
these**, because a missing licence tag means unknown, not unrestricted.

| Dataset | Content | Format | Licence (as declared) | Use |
| --- | --- | --- | --- | --- |
| [`UniverseTBD/arxiv-qa-astro-ph`](https://huggingface.co/datasets/UniverseTBD/arxiv-qa-astro-ph) | 10,356 question/answer pairs generated from astro-ph | parquet, `question`/`answer` | none declared | Candidate SFT material after review |
| [`AstroMLab/Astrobench_MCQ_v1_Public`](https://huggingface.co/datasets/AstroMLab/Astrobench_MCQ_v1_Public) | 4,425 multiple-choice questions from *Annual Review of Astronomy and Astrophysics* | parquet | derived review articles; check terms | Evaluation only, not SFT |
| [`AstroMLab/USAAAO_QA`](https://huggingface.co/datasets/AstroMLab/USAAAO_QA) | USAAAO 2017–2019 open-ended problems with worked solutions | JSONL + figures, gated | `other` | Evaluation; small SFT only if terms allow |
| [`Ashu9675/space-llm-training-data`](https://huggingface.co/datasets/Ashu9675/space-llm-training-data) | ~1.27B tokens of papers, arXiv abstracts and web text | plain text | `mit` | Continued pretraining, **not** this LoRA pipeline |
| [`guigux/astro_qa_fr_0.1`](https://huggingface.co/datasets/guigux/astro_qa_fr_0.1) | ~5k ChatGPT-generated French astrophysics pairs | parquet | none declared | Language mismatch; skip |
| [`SKIML-ICL/astro_qa`](https://huggingface.co/datasets/SKIML-ICL/astro_qa) | Small astronomy QA set | parquet | none declared | Unverified provenance |

Two things are worth stating plainly. First, a generated Q/A dataset is not a
reviewed dataset: an answer produced by another model can be fluent and wrong,
and Kestrel will train on whatever is imported. Second, benchmark sets derived
from review articles are for measuring, not for training; their licence usually
does not permit the latter even when the file is publicly downloadable.

### Import workflow

Nothing in the app downloads anything. Materialise the dataset locally, then
convert it to Kestrel's schema:

```python
from datasets import load_dataset
load_dataset("UniverseTBD/arxiv-qa-astro-ph", split="train").to_json("astro-qa.jsonl")
```

```sh
python3 scripts/import_astronomy_dataset.py \
  --input astro-qa.jsonl --output kestrel-astro.jsonl \
  --license "<the licence you verified>" --origin UniverseTBD/arxiv-qa-astro-ph

# Inspect before spending GPU time.
python3 scripts/train_persona.py --dataset kestrel-astro.jsonl \
  --domain astronomy --allow-imported --automatic-evaluation --validate-only

python3 scripts/train_persona.py --dataset kestrel-astro.jsonl \
  --domain astronomy --allow-imported --include-seeds \
  --model /path/to/Qwen3-4B-Base-bnb-4bit --output /path/to/new-adapter \
  --approve-training --automatic-evaluation --inference-model /path/to/model.gguf
```

The importer deduplicates on normalized prompts (first answer wins), skips rows
without both fields, skips answers shorter than `--min-answer-chars` (default
40) and fields over 4000 bytes, caps the output at 1000 examples, records the
licence and origin, writes owner-only, and refuses to overwrite an existing
file.

## Verification

```sh
ctest --test-dir build-linux --output-on-failure          # includes the Python trainer suite
python3 -m unittest tests.train_persona_tests -v
./build-linux/kestrel --print-runtime                     # reports "specialism : astronomy"
```

The GPT/ONNX mock path and the real GPU trainer still require the local training
environment described in [LEARNING.md](LEARNING.md); nothing here removes those
checksum and lineage gates.

## Limitations

- The prompt and the tools make Kestrel careful, not omniscient. The 4B base
  model's ceiling is still the ceiling of the adapter trained on it.
- The astronomy tools compute; they do not identify objects. No catalogue
  lookup is performed, so "what is at this position" is not a tool answer.
- The domain gate compares text similarity on a handful of held-out prompts. It
  can reject a regression, and it cannot promise the field improved.
- Imported datasets are the user's licensing responsibility. The importer
  records the declared licence; it does not enforce it.
