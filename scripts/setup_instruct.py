#!/usr/bin/env python3
"""Install the optional local Instruct model; never invoked by ordinary chat.

Uses only the Python standard library. The immutable revision and LFS checksum
were read from the publisher's Hugging Face model API. This verifies bytes and
provenance, not model quality or adapter compatibility.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import tempfile
import urllib.request

MODEL = {
    "repository": "unsloth/Qwen3-4B-Instruct-2507-GGUF",
    "revision": "a06e946bb6b655725eafa393f4a9745d460374c9",
    "filename": "Qwen3-4B-Instruct-2507-Q4_K_M.gguf",
    "bytes": 2497281120,
    "sha256": "3605803b982cb64aead44f6c1b2ae36e3acdb41d8e46c8a94c6533bc4c67e597",
    "base_model": "Qwen/Qwen3-4B-Instruct-2507",
}


def verify(path: Path, record: dict = MODEL) -> None:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as source:
        if source.read(4) != b"GGUF":
            raise ValueError("Not a GGUF model. No model was installed.")
        source.seek(0)
        for block in iter(lambda: source.read(4 * 1024 * 1024), b""):
            size += len(block)
            digest.update(block)
    if size != record["bytes"] or digest.hexdigest() != record["sha256"]:
        raise ValueError("Model size or SHA-256 mismatch. Re-download the pinned model; existing weights were not replaced.")


def install(directory: Path, verify_only: bool = False) -> Path:
    path = directory / MODEL["filename"]
    if path.exists() or verify_only:
        verify(path)
    else:
        directory.mkdir(parents=True, exist_ok=True)
        url = f"https://huggingface.co/{MODEL['repository']}/resolve/{MODEL['revision']}/{MODEL['filename']}"
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=directory, prefix=".instruct-", suffix=".part", delete=False) as output:
                temporary = Path(output.name)
                with urllib.request.urlopen(url, timeout=60) as response:
                    if not response.geturl().startswith("https://"):
                        raise ValueError("Refusing an unencrypted model download.")
                    size = 0
                    for block in iter(lambda: response.read(4 * 1024 * 1024), b""):
                        size += len(block)
                        if size > MODEL["bytes"]:
                            raise ValueError("Download exceeded the pinned model size.")
                        output.write(block)
                output.flush()
                os.fsync(output.fileno())
            verify(temporary)
            # Do not overwrite a model created by another installer meanwhile.
            if path.exists():
                verify(path)
            else:
                temporary.replace(path)
        finally:
            if temporary is not None:
                temporary.unlink(missing_ok=True)
    manifest = path.with_suffix(".manifest.json")
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=directory,
                                     prefix=".manifest-", delete=False) as output:
        temporary = Path(output.name)
        try:
            json.dump(MODEL, output, indent=2)
            output.write("\n")
            output.flush()
            os.fsync(output.fileno())
        except BaseException:
            temporary.unlink(missing_ok=True)
            raise
    try:
        temporary.replace(manifest)
    finally:
        temporary.unlink(missing_ok=True)
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--directory", type=Path, default=Path(__file__).resolve().parents[1] / "build" / "models")
    parser.add_argument("--verify-only", action="store_true", help="Check existing bytes without downloading")
    args = parser.parse_args()
    path = install(args.directory, args.verify_only)
    print(f"Verified local Instruct model: {path}")
    print("Choose this GGUF in Kestrel. Base LoRA adapters must not be applied to it.")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        raise SystemExit(f"Instruct setup failed: {error}")
