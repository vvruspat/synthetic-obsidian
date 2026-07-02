#!/usr/bin/env python3
"""Batch-generate full backing-vocal DSL files for a directory tree."""

from __future__ import annotations

import argparse
import concurrent.futures
import shutil
from pathlib import Path

from generate_backing_vocals_dsl import generate_backing, insert_backing, parse_song, render_backing_yaml


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--seed-root", type=Path, help="Existing generated files to copy into output first.")
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--overwrite", action="store_true")
    return parser.parse_args()


def generate_one(args: tuple[str, str, str, bool]) -> tuple[str, bool, str]:
    source_text, input_root_text, output_root_text, overwrite = args
    source = Path(source_text)
    input_root = Path(input_root_text)
    output_root = Path(output_root_text)
    relative = source.relative_to(input_root)
    destination = output_root / relative
    if destination.exists() and not overwrite:
        return str(relative), True, "exists"
    try:
        text = sanitize_yaml_scalars(source.read_text(encoding="utf-8"))
        song = parse_song(text)
        if not song.chords or not song.lead:
            return str(relative), False, "missing chords or lead_vocal"
        backing = generate_backing(song)
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(insert_backing(text, render_backing_yaml(backing)), encoding="utf-8")
        return str(relative), True, "generated"
    except Exception as error:  # noqa: BLE001 - batch report should keep going.
        return str(relative), False, f"{type(error).__name__}: {error}"


def sanitize_yaml_scalars(text: str) -> str:
    """Quote legacy DSL scalar values that contain YAML-hostile colon-space text."""
    output: list[str] = []
    for line in text.splitlines():
        stripped = line.lstrip(" ")
        prefix_len = len(line) - len(stripped)
        if stripped.startswith("- ") and ": " in stripped:
            head, value = stripped.split(": ", 1)
            prefix = " " * prefix_len + head + ": "
        elif ": " in stripped and not stripped.startswith("- "):
            head, value = stripped.split(": ", 1)
            prefix = " " * prefix_len + head + ": "
        else:
            output.append(line)
            continue
        if should_quote_scalar(value):
            output.append(prefix + quote_scalar(value))
        else:
            output.append(line)
    return "\n".join(output).rstrip() + "\n"


def should_quote_scalar(value: str) -> bool:
    value = value.strip()
    if not value or value in {"null", "true", "false"}:
        return False
    if value[0] in {"'", '"', "[", "{", "|", ">"}:
        return False
    return ": " in value or " #" in value


def quote_scalar(value: str) -> str:
    return '"' + value.strip().replace("\\", "\\\\").replace('"', '\\"') + '"'


def copy_seed(seed_root: Path, output_root: Path, overwrite: bool) -> int:
    copied = 0
    for source in sorted(seed_root.rglob("*.yaml")):
        relative = source.relative_to(seed_root)
        destination = output_root / relative
        if destination.exists() and not overwrite:
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, destination)
        copied += 1
    return copied


def main() -> None:
    args = parse_args()
    input_root = args.input_root.expanduser().resolve()
    output_root = args.output_root.expanduser().resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    if args.seed_root:
        copied = copy_seed(args.seed_root.expanduser().resolve(), output_root, args.overwrite)
        print(f"Copied {copied} seed files")

    sources = sorted(input_root.rglob("*.yaml"))
    tasks = [(str(source), str(input_root), str(output_root), args.overwrite) for source in sources]
    ok = 0
    failed = 0
    skipped = 0
    with concurrent.futures.ProcessPoolExecutor(max_workers=args.workers) as executor:
        for index, (relative, success, message) in enumerate(executor.map(generate_one, tasks), start=1):
            if success:
                ok += 1
                if message == "exists":
                    skipped += 1
            else:
                failed += 1
                print(f"FAILED {relative}: {message}")
            if index % 50 == 0 or index == len(tasks):
                print(f"Progress {index}/{len(tasks)} ok={ok} skipped={skipped} failed={failed}", flush=True)
    print(f"Done ok={ok} skipped={skipped} failed={failed} output={output_root}")


if __name__ == "__main__":
    main()
