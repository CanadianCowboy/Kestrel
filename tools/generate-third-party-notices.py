#!/usr/bin/env python3
"""Generate THIRD-PARTY-NOTICES.md and licenses/ for everything Kestrel ships.

Run it from the repository root:

    python tools/generate-third-party-notices.py

Why this is a generator and not a checked-in list: the set of things Kestrel
redistributes is decided by a virtual environment and a handful of native
trees, and a list written by hand describes the set as it was on the day
someone wrote it. A package added to the voice environment, or a runtime
bumped, would ship unlisted and the gap would only surface during a
compliance review. Deriving the list from the environment means it is wrong
the moment reality changes and cannot stay wrong.

The native dependencies are the exception: they live outside the repository,
at paths that differ per machine, so they are declared in NATIVE_COMPONENTS
below and the file each one's licence came from is recorded. That table is
small, hand-maintained, and every entry says where its text is -- which is the
difference between this and a list nobody trusts.
"""
import email.parser
import hashlib
import os
import re
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
VENV = REPO / ".kestrel-voice" / "Lib" / "site-packages"
OUT_MD = REPO / "THIRD-PARTY-NOTICES.md"
OUT_DIR = REPO / "licenses"

# Packages that are installed in the development venv but deliberately left out
# of the distributed package. This list and the /XD list in
# tools/package-desktop.bat have to agree: the packager fails the run if one of
# these turns up in the stage directory, and this file says why it should not be
# there. If you add a name here, add it there too.
NOT_SHIPPED = {"piper-tts"}

# Native components linked into kestrel.exe or shipped beside it. Each entry
# names the licence and, where one exists on the machine that built this, the
# file the text was taken from. `required` is False for a component this build
# did not link, whose row is still emitted so the reader can see it was
# considered rather than missed.
NATIVE_COMPONENTS = [
    {
        "name": "llama.cpp",
        "what": "GGUF inference, linked into kestrel.exe; llama.dll, ggml*.dll",
        "licence": "MIT",
        "from": os.environ.get("KESTREL_LLAMA_CPP_LICENCE_FILE", ""),
    },
    {
        "name": "ONNX Runtime",
        "what": "inference runtime, onnxruntime.dll and the execution providers",
        "licence": "MIT",
        "from": os.environ.get("KESTREL_ORT_LICENCE_FILE", ""),
    },
    {
        "name": "ONNX Runtime GenAI",
        "what": "generative loop over ONNX models, onnxruntime-genai*.dll",
        "licence": "MIT",
        "from": os.environ.get("KESTREL_GENAI_LICENCE_FILE", ""),
    },
    {
        "name": "Qt 6",
        "what": "application framework, deployed by windeployqt; LGPL-3.0-only "
                "unless the Qt commercial licence applies",
        "licence": "LGPL-3.0-only OR LicenseRef-Qt-Commercial",
        "from": os.environ.get("KESTREL_QT_LICENCE_FILE", ""),
    },
    {
        "name": "NVIDIA CUDA Runtime",
        "what": "cudart, redistributed under the CUDA EULA",
        "licence": "LicenseRef-NVIDIA-CUDA-EULA",
        "from": os.environ.get("KESTREL_CUDA_LICENCE_FILE", ""),
    },
    {
        "name": "Microsoft Visual C++ Runtime",
        "what": "msvcp140/vcruntime140 redistributables",
        "licence": "LicenseRef-MSVC-Redistributable",
        "from": "",
    },
    {
        "name": "TensorRT",
        "what": "optional engine tooling (kestrel-engine-build); not linked into "
                "kestrel.exe and not shipped",
        "licence": "LicenseRef-NVIDIA-TensorRT",
        "from": "",
    },
]

# Model weights are not code, and no package manager reports their licence, so
# they are declared rather than derived. Getting this one wrong is how a model
# with a restricted licence ends up in a redistributable package.
MODEL_COMPONENTS = [
    {
        "name": "Kokoro v1.0 (kokoro-v1.0.onnx)",
        "what": "neural voice, in .kestrel-voice/models and shipped in the desktop package",
        "licence": "Apache-2.0",
    },
    {
        "name": "Piper voices",
        "what": "alternative voices in .kestrel-voice/piper; NOT shipped, see the "
                "note below on piper-tts",
        "licence": "per-voice; MIT for most, some carry their own",
    },
]

# Licence expressions seen in the wild that are not a bare SPDX id. Left as
# written rather than normalised, because collapsing "BSD-3-Clause AND MIT" to
# one of them would be a false statement about what is licensed under what.
AMBIGUOUS = re.compile(r"^(?![\w.-]+$|LicenseRef-)", re.IGNORECASE)


def read_metadata(path):
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            return email.parser.Parser().parse(handle)
    except OSError:
        return None


# Trove classifiers spell the copyleft licences the long way round. Mapping them
# to SPDX is not cosmetic: the copyleft check below keys off the value, and
# "GNU General Public License v3 or later (GPLv3+)" does not start with "GPL",
# so a GPL component can pass through this file unremarked.
CLASSIFIER_SPDX = {
    "GNU General Public License v3 or later (GPLv3+)": "GPL-3.0-or-later",
    "GNU General Public License v3 (GPLv3)": "GPL-3.0-only",
    "GNU General Public License v2 or later (GPLv2+)": "GPL-2.0-or-later",
    "GNU Lesser General Public License v3 or later (LGPLv3+)":
        "LGPL-3.0-or-later",
    "MIT License": "MIT",
    "Apache Software License": "Apache-2.0",
    "BSD License": "BSD-3-Clause",
}


def classifier_licence(meta):
    for classifier in meta.get_all("Classifier", []):
        if not classifier.startswith("License ::"):
            continue
        tail = classifier.rsplit(" :: ", 1)[-1].strip()
        if tail in CLASSIFIER_SPDX:
            return CLASSIFIER_SPDX[tail]
    return ""


def licence_of(meta):
    """SPDX expression if the metadata has one, else the free-text field."""
    expression = meta.get("License-Expression", "").strip()
    if expression:
        return expression
    from_classifier = classifier_licence(meta)
    free = meta.get("License", "").strip()
    squashed = re.sub(r"\s+", " ", free)
    # Some packages put the entire licence text in the License field. Dumping
    # that into a markdown table produces a single unreadable row that is also
    # useless for the copyleft check, so treat it as absent and fall back to
    # the classifier, which is the only place a usable name is left.
    if len(squashed) > 200:
        return from_classifier or "NOT DECLARED"
    if not squashed:
        if from_classifier:
            return from_classifier
        for classifier in meta.get_all("Classifier", []):
            if classifier.startswith("License ::"):
                return classifier.rsplit(" :: ", 1)[-1]
        return ""
    # Normalise the handful of spellings that appear, and leave anything else
    # alone rather than guessing: a wrong SPDX id is worse than a plain name.
    for pattern, spdx in (
        (r"^MIT License$", "MIT"),
        (r"^MIT$", "MIT"),
        (r"^Apache 2\.0$", "Apache-2.0"),
        (r"^BSD 3-Clause License$", "BSD-3-Clause"),
        (r"^BSD 2-Clause License$", "BSD-2-Clause"),
        (r"^GNU Lesser General Public License v3", "LGPL-3.0"),
    ):
        if re.match(pattern, squashed, re.IGNORECASE):
            return spdx
    return squashed


def licence_files(dist_info):
    """Every licence text the distribution shipped, by absolute path."""
    found = []
    meta = read_metadata(dist_info / "METADATA")
    declared = meta.get_all("License-File", []) if meta else []
    for relative in declared:
        candidate = dist_info / relative
        if candidate.is_file():
            found.append(candidate)
    if not found:
        for candidate in sorted(dist_info.glob("LICENSE*")) + sorted(
            dist_info.glob("COPYING*")
        ):
            if candidate.is_file():
                found.append(candidate)
        licenses_dir = dist_info / "licenses"
        if licenses_dir.is_dir():
            found.extend(p for p in sorted(licenses_dir.rglob("*")) if p.is_file())
    return found


def safe_name(text, limit=60):
    """A filesystem-safe fragment, truncated.

    The truncation is not cosmetic. Some distributions put their entire licence
    in the free-text `License:` field rather than a `License-Expression`, so the
    "licence" a row reports is a paragraph and building a filename from it
    produces a path Windows rejects outright -- and it does so on the first such
    package, which is how this script failed the first time it ran. A short name
    plus a digest of the full string keeps the mapping unique without ever
    depending on how long the field happens to be.
    """
    cleaned = re.sub(r"[^A-Za-z0-9._-]+", "-", text).strip("-") or "UNKNOWN"
    if len(cleaned) <= limit:
        return cleaned
    digest = hashlib.sha1(text.encode("utf-8", "replace")).hexdigest()[:8]
    return cleaned[: limit - 9].rstrip("-") + "-" + digest


def collect_python_packages():
    rows = []
    if not VENV.is_dir():
        return rows
    for dist_info in sorted(VENV.glob("*.dist-info")):
        meta = read_metadata(dist_info / "METADATA")
        if meta is None:
            continue
        rows.append(
            {
                "name": meta.get("Name", dist_info.name),
                "version": meta.get("Version", "?"),
                "licence": licence_of(meta) or "NOT DECLARED",
                "texts": licence_files(dist_info),
                "summary": meta.get("Summary", "").strip(),
                "home": meta.get("Home-page", "").strip(),
            }
        )
    return rows


def copy_texts(rows, components, written):
    """Copies each distinct licence text into licenses/ and returns what it used."""
    used = {}
    for row in rows:
        for text in row["texts"]:
            try:
                body = text.read_text(encoding="utf-8", errors="replace")
            except OSError:
                continue
            if not body.strip():
                continue
            key = (row["licence"], body)
            if key in written:
                used.setdefault(row["licence"], set()).add(written[key])
                continue
            filename = f"{safe_name(row['licence'])}-{safe_name(row['name'])}.txt"
            target = OUT_DIR / filename
            target.write_text(body, encoding="utf-8")
            written[key] = filename
            used.setdefault(row["licence"], set()).add(filename)
    for component in components:
        source = component.get("from", "")
        if not source or not os.path.isfile(source):
            continue
        body = open(source, "r", encoding="utf-8", errors="replace").read()
        if not body.strip():
            continue
        key = (component["licence"], body)
        if key in written:
            used.setdefault(component["licence"], set()).add(written[key])
            continue
        filename = f"{safe_name(component['licence'])}-{safe_name(component['name'])}.txt"
        (OUT_DIR / filename).write_text(body, encoding="utf-8")
        written[key] = filename
        used.setdefault(component["licence"], set()).add(filename)
    return used


def licence_order(text):
    """Copyleft last, so a glance at the list surfaces the obligations."""
    order = ["MIT", "BSD", "Apache", "PSF", "ISC", "Zlib", "MPL", "LGPL", "GPL"]
    for index, prefix in enumerate(order):
        if text.upper().startswith(prefix.upper()):
            return index
    return len(order)


def main():
    rows = collect_python_packages()
    if not rows:
        print("warning: no packages found in", VENV)
        print("         the Python half of the notice will be empty.")

    OUT_DIR.mkdir(exist_ok=True)
    for stale in OUT_DIR.glob("*.txt"):
        stale.unlink()

    written = {}
    by_licence = copy_texts(rows, NATIVE_COMPONENTS, written)

    undeclared = [r for r in rows if r["licence"] == "NOT DECLARED"]
    untexted = [
        r for r in rows
        if r["licence"] != "NOT DECLARED"
        and r["licence"] not in by_licence
    ]

    lines = []
    lines.append("# Third-party notices")
    lines.append("")
    lines.append(
        "Generated by `tools/generate-third-party-notices.py`. Do not edit by "
        "hand: regenerate it, and the regeneration is the check that this file "
        "still describes what the build actually ships."
    )
    lines.append("")
    lines.append(
        "Kestrel is distributed as a desktop application and carries the "
        "components below inside it. Each is used unmodified and remains the "
        "property of its authors; the licence named here governs it."
    )
    lines.append("")

    lines.append("## Native components")
    lines.append("")
    lines.append("Linked into `kestrel.exe` or deployed beside it.")
    lines.append("")
    lines.append("| Component | Used for | Licence |")
    lines.append("| --- | --- | --- |")
    for component in NATIVE_COMPONENTS:
        lines.append(
            f"| {component['name']} | {component['what']} | "
            f"`{component['licence']}` |"
        )
    lines.append("")

    lines.append("## Python packages")
    lines.append("")
    lines.append(
        f"From `.kestrel-voice`, the virtual environment that runs the local "
        f"voice. {len(rows)} distributions."
    )
    lines.append("")
    lines.append("| Package | Version | Licence |")
    lines.append("| --- | --- | --- |")
    for row in sorted(rows, key=lambda r: (licence_order(r["licence"]), r["name"].lower())):
        lines.append(f"| {row['name']} | {row['version']} | `{row['licence']}` |")
    lines.append("")

    lines.append("## Model weights")
    lines.append("")
    lines.append("| Model | Used for | Licence |")
    lines.append("| --- | --- | --- |")
    for component in MODEL_COMPONENTS:
        lines.append(
            f"| {component['name']} | {component['what']} | "
            f"`{component['licence']}` |"
        )
    lines.append("")

    gpl = [r for r in rows if r["licence"].upper().startswith("GPL")
           and "LGPL" not in r["licence"].upper()]
    if gpl:
        dropped = [r for r in gpl if r["name"] in NOT_SHIPPED]
        shipped = [r for r in gpl if r["name"] not in NOT_SHIPPED]
        lines.append("## Copyleft components")
        lines.append("")
        if dropped:
            lines.append("### In the development environment, not shipped")
            lines.append("")
            for row in dropped:
                lines.append(
                    f"- **{row['name']} {row['version']}** -- `{row['licence']}`. "
                    "Nothing in the build imports it. "
                    "`tools/package-desktop.bat` excludes it from the `Lib` copy "
                    "and fails the packaging run if it is still there afterwards."
                )
            lines.append("")
        if shipped:
            lines.append("### Shipped, with obligations attached")
            lines.append("")
            for row in shipped:
                lines.append(
                    f"- **{row['name']} {row['version']}** -- `{row['licence']}`, "
                    "redistributed in the package."
                )
            lines.append("")
            lines.append(
                "These are a genuine GPL-3.0 dependency and are not "
                "discretionary. `kokoro-onnx` requires `phonemizer-fork` to make "
                "the voice speak, so it cannot be dropped while Kokoro is the "
                "voice. It is the one thing that stops this package being "
                "entirely permissive, and it is the reason Kestrel has not been "
                "relicensed to GPL-3.0: doing so would solve a problem the "
                "process boundary already solves, at the cost of the permissive "
                "licence for every other user of the code."
            )
            lines.append("")
            lines.append(
                "**What this does and does not require.** Kestrel invokes the "
                "voice as a separate `python.exe` process and exchanges JSON over "
                "a pipe. Copyleft reaches a *derivative work*; a program run as a "
                "separate process and talked to over stdin/stdout is a separate "
                "work, so Kestrel's own MIT licence is not forced to change. What "
                "GPL-3.0 does require of anyone redistributing this package is on "
                "that component itself: convey its licence (see `licenses/`) and "
                "its Corresponding Source, or a written offer of it. A licence "
                "file alone does not discharge that."
            )
            lines.append("")
            lines.append(
                "This is the ordinary reading rather than legal advice, and it "
                "holds only while the boundary stays where it is. Linking the ONNX "
                "runtime into `kestrel.exe` and calling it in-process would change "
                "the analysis, and so would a plugin that embeds it. Re-visit this "
                "section if either happens."
            )
            lines.append("")

    lines.append("## Licence texts")
    lines.append("")
    if by_licence:
        lines.append("Full texts are in `licenses/`, one file per component.")
        lines.append("")
        for licence in sorted(by_licence, key=licence_order):
            for filename in sorted(by_licence[licence]):
                lines.append(f"- `{filename}` -- {licence}")
    else:
        lines.append(
            "No licence texts were found. If this section is empty the generator "
            "ran where the components were not present; run it on a machine that "
            "has them, or point the `KESTREL_*_LICENCE_FILE` variables at the "
            "texts before running it."
        )
    lines.append("")

    if undeclared:
        lines.append("## Gaps")
        lines.append("")
        lines.append(
            "Distributions that install no licence metadata. They are listed here "
            "because a component whose terms are unknown cannot be declared:"
        )
        lines.append("")
        for row in undeclared:
            lines.append(f"- {row['name']} {row['version']}")
        lines.append("")
    if untexted:
        lines.append("## Licences named but not found on disk")
        lines.append("")
        lines.append(
            "Declared by the package, with no licence text shipped alongside it. "
            "The identifier is reported from its own metadata; the text was not "
            "available to copy."
        )
        lines.append("")
        for row in untexted:
            lines.append(f"- {row['name']} {row['version']} -- `{row['licence']}`")
        lines.append("")

    OUT_MD.write_text("\n".join(lines) + "\n", encoding="utf-8")

    print(f"{OUT_MD.relative_to(REPO)}: {len(rows)} python packages, "
          f"{len(NATIVE_COMPONENTS)} native components")
    print(f"{OUT_DIR.relative_to(REPO)}/: {len(written)} licence texts")
    for label, group in (("no licence metadata", undeclared),
                         ("declared but no text found", untexted)):
        if group:
            print(f"  {len(group)} with {label}: "
                  + ", ".join(sorted(r["name"] for r in group)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
