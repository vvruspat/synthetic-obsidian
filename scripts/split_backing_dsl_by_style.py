#!/usr/bin/env python3
"""Split full backing-vocal DSL YAML files into one file per backing style."""

from __future__ import annotations

import argparse
import re
import shutil
from pathlib import Path


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--overwrite", action="store_true")
    parser.add_argument("--limit", type=int, default=0)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    input_root = args.input_root.expanduser().resolve()
    output_root = args.output_root.expanduser().resolve()
    if output_root.exists() and args.overwrite:
        shutil.rmtree(output_root)
    output_root.mkdir(parents=True, exist_ok=True)

    files = sorted(input_root.rglob("*.yaml"))
    if args.limit > 0:
        files = files[: args.limit]

    written = 0
    failed = 0
    for index, source in enumerate(files, start=1):
        try:
            written += split_file(source, input_root, output_root)
        except Exception as error:  # noqa: BLE001 - keep batch running and report path.
            failed += 1
            print(f"FAILED {source}: {type(error).__name__}: {error}")
        if index % 100 == 0 or index == len(files):
            print(f"Progress {index}/{len(files)} written={written} failed={failed}", flush=True)
    print(f"Done files={len(files)} style_files={written} failed={failed} output={output_root}")


def split_file(source: Path, input_root: Path, output_root: Path) -> int:
    text = source.read_text(encoding="utf-8")
    before, backing_block, after = split_backing_block(text)
    sets = split_backing_sets(backing_block)
    if len(sets) != 35:
        raise ValueError(f"expected 35 backing sets, got {len(sets)}")

    relative = source.relative_to(input_root)
    song_dir = output_root / relative.with_suffix("")
    song_dir.mkdir(parents=True, exist_ok=True)
    for order, backing_set in enumerate(sets, start=1):
        style_id = style_id_from_set(backing_set) or f"STYLE_{order:02d}"
        destination = song_dir / f"{order:02d}_{safe_filename(style_id)}.yaml"
        destination.write_text(before.rstrip() + "\n" + render_backing_set(backing_set) + after, encoding="utf-8")
    return len(sets)


def split_backing_block(text: str) -> tuple[str, str, str]:
    marker = "\n  backing_vocals:\n"
    start = text.find(marker)
    if start < 0:
        raise ValueError("missing tracks.backing_vocals")
    analysis = text.find("\nanalysis:", start)
    if analysis < 0:
        raise ValueError("missing analysis block after backing_vocals")
    before = text[:start]
    backing = text[start + 1 : analysis]
    after = text[analysis:]
    return before, backing, after


def split_backing_sets(backing_block: str) -> list[list[str]]:
    lines = backing_block.splitlines()
    if not lines or lines[0] != "  backing_vocals:":
        raise ValueError("bad backing_vocals header")
    sets: list[list[str]] = []
    current: list[str] = []
    for line in lines[1:]:
        if line.startswith("    - id: ") and current:
            sets.append(current)
            current = [line]
        elif line.startswith("    - id: "):
            current = [line]
        elif current:
            current.append(line)
    if current:
        sets.append(current)
    return sets


def render_backing_set(backing_set: list[str]) -> str:
    return "  backing_vocals:\n" + "\n".join(backing_set) + "\n"


def style_id_from_set(backing_set: list[str]) -> str:
    for line in backing_set[:4]:
        if line.startswith("    - id: "):
            return line.removeprefix("    - id: ").strip().strip('"')
    return ""


def safe_filename(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9_.-]+", "_", value).strip("._")
    return value or "style"


if __name__ == "__main__":
    main()
