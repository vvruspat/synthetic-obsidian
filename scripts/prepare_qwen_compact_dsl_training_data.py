#!/usr/bin/env python3
"""Prepare compact single-style DSL chat data for Qwen LoRA training."""

from __future__ import annotations

import argparse
import json
import random
from pathlib import Path


SYSTEM = (
    "You generate Synthetic Obsidian compact backing-vocal DSL JSON. "
    "Return only JSON with backing_vocals_compact. Do not include markdown or explanations."
)

CHORD_FIELDS = ["id", "start", "duration", "bar", "beat", "chord", "root", "quality", "bass"]
LEAD_FIELDS = ["id", "start", "duration", "bar", "beat", "pitch", "midi_note", "velocity", "syllable", "phrase_id", "chord_ref"]
NOTE_FIELDS = ["id", "start", "duration", "bar", "beat", "pitch", "midi_note", "velocity", "syllable", "phrase_id", "chord_ref", "source_lead_ref"]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--max-lead-notes", type=int, default=32)
    parser.add_argument("--max-chords", type=int, default=96)
    parser.add_argument("--valid-ratio", type=float, default=0.05)
    parser.add_argument("--seed", type=int, default=42)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    input_root = args.input_root.expanduser().resolve()
    output_root = args.output_root.expanduser().resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    files = sorted(input_root.rglob("*.yaml"))
    if args.limit > 0:
        files = files[: args.limit]
    rows = []
    for index, path in enumerate(files, start=1):
        row = build_row(path, input_root, args.max_lead_notes, args.max_chords, index)
        if row is not None:
            rows.append(row)
    write_split(rows, output_root / "train.jsonl", output_root / "valid.jsonl", args.valid_ratio, random.Random(args.seed))
    manifest = {"examples": len(rows), "limit": args.limit, "max_lead_notes": args.max_lead_notes, "max_chords": args.max_chords}
    (output_root / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps(manifest, indent=2))


def build_row(path: Path, input_root: Path, max_lead: int, max_chords: int, index: int) -> dict[str, object] | None:
    text = path.read_text(encoding="utf-8")
    meta = parse_key_values(extract_top_level_block(text, "meta:"))
    chords = parse_items(extract_tracks_subblock(text, "chords:"))[:max_chords]
    lead = parse_items(extract_tracks_subblock(text, "lead_vocal:"))[:max_lead]
    lead_refs = {str(item.get("id", "")) for item in lead}
    backing_sets = parse_backing_sets(extract_tracks_subblock(text, "backing_vocals:"))
    if not chords or not lead or len(backing_sets) != 1:
        return None
    backing = compact_backing(backing_sets[0], lead_refs)
    if not backing["parts"]:
        return None
    source = {
        "meta": meta,
        "chord_schema": CHORD_FIELDS,
        "chords": [[item.get(field) for field in CHORD_FIELDS] for item in chords],
        "lead_vocal_schema": LEAD_FIELDS,
        "lead_vocal": [[item.get(field) for field in LEAD_FIELDS] for item in lead],
    }
    style_id = backing["id"]
    style_name = backing["name"]
    user = {
        "task": "GENERATE_BACKING_VOCAL_COMPACT",
        "requested_style_id": style_id,
        "requested_style_name": style_name,
        "source_file": path.relative_to(input_root).as_posix(),
        "song": source,
    }
    assistant = {"backing_vocals_compact": [backing]}
    return {
        "messages": [
            {"role": "system", "content": SYSTEM},
            {"role": "user", "content": json.dumps(user, ensure_ascii=False, separators=(",", ":"))},
            {"role": "assistant", "content": json.dumps(assistant, ensure_ascii=False, separators=(",", ":"))},
        ],
        "metadata": {
            "task": "generate_backing_vocal_compact",
            "source_file": path.relative_to(input_root).as_posix(),
            "style_id": style_id,
            "style_name": style_name,
            "example_index": index,
        },
    }


def compact_backing(backing: dict[str, object], lead_refs: set[str]) -> dict[str, object]:
    output_parts = []
    for part in backing.get("parts", []):
        notes = [note for note in part.get("notes", []) if str(note.get("source_lead_ref", "")) in lead_refs]
        if not notes:
            continue
        output_parts.append(
            {
                "id": part.get("id"),
                "role": part.get("role"),
                "strategy": part.get("strategy"),
                "range": part.get("range"),
                "notes": [[note.get(field) for field in NOTE_FIELDS] for note in notes],
            }
        )
    return {
        "id": backing.get("id"),
        "name": backing.get("name"),
        "description": backing.get("description"),
        "confidence": backing.get("confidence"),
        "note_schema": NOTE_FIELDS,
        "parts": output_parts,
    }


def extract_top_level_block(text: str, header: str) -> list[str]:
    lines = text.splitlines()
    out = []
    active = False
    for line in lines:
        if line == header:
            active = True
            continue
        if active and line and not line.startswith(" "):
            break
        if active:
            out.append(line)
    return out


def extract_tracks_subblock(text: str, header: str) -> list[str]:
    lines = text.splitlines()
    out = []
    in_tracks = False
    active = False
    for line in lines:
        if line == "tracks:":
            in_tracks = True
            continue
        if in_tracks and line and not line.startswith(" "):
            break
        if in_tracks and line == f"  {header}":
            active = True
            continue
        if active and line.startswith("  ") and not line.startswith("    "):
            break
        if active:
            out.append(line)
    return out


def parse_key_values(lines: list[str]) -> dict[str, object]:
    data = {}
    for line in lines:
        stripped = line.strip()
        if ": " in stripped:
            key, value = stripped.split(": ", 1)
            data[key] = parse_scalar(value)
    return data


def parse_items(lines: list[str]) -> list[dict[str, object]]:
    items = []
    current = None
    for line in lines:
        stripped = line.strip()
        if line.startswith("    - "):
            current = {}
            items.append(current)
            rest = stripped[2:]
            if ": " in rest:
                key, value = rest.split(": ", 1)
                current[key] = parse_scalar(value)
        elif current is not None and ": " in stripped:
            key, value = stripped.split(": ", 1)
            current[key] = parse_scalar(value)
    return items


def parse_backing_sets(lines: list[str]) -> list[dict[str, object]]:
    sets = []
    current = None
    current_part = None
    current_note = None
    for line in lines:
        stripped = line.strip()
        if line.startswith("    - id: "):
            current = {"id": parse_scalar(stripped.removeprefix("- id: ")), "parts": []}
            sets.append(current)
            current_part = None
            current_note = None
        elif current is not None and line.startswith("      ") and not line.startswith("        ") and ": " in stripped:
            key, value = stripped.split(": ", 1)
            current[key] = parse_scalar(value)
        elif current is not None and line.startswith("        - id: "):
            current_part = {"id": parse_scalar(stripped.removeprefix("- id: ")), "notes": []}
            current["parts"].append(current_part)
            current_note = None
        elif current_part is not None and line.startswith("          ") and not line.startswith("            ") and ": " in stripped:
            key, value = stripped.split(": ", 1)
            current_part[key] = parse_scalar(value)
        elif current_part is not None and line.startswith("            - id: "):
            current_note = {"id": parse_scalar(stripped.removeprefix("- id: "))}
            current_part["notes"].append(current_note)
        elif current_note is not None and line.startswith("              ") and ": " in stripped:
            key, value = stripped.split(": ", 1)
            current_note[key] = parse_scalar(value)
    return sets


def parse_scalar(value: str) -> object:
    value = value.strip()
    if value == "null":
        return None
    if value.startswith('"') and value.endswith('"'):
        return value[1:-1].replace('\\"', '"').replace("\\\\", "\\")
    if value.startswith("[") and value.endswith("]"):
        return [parse_scalar(part.strip()) for part in value[1:-1].split(",") if part.strip()]
    try:
        if "." in value:
            return float(value)
        return int(value)
    except ValueError:
        return value


def write_split(rows: list[dict[str, object]], train_path: Path, valid_path: Path, valid_ratio: float, rng: random.Random) -> None:
    indices = list(range(len(rows)))
    rng.shuffle(indices)
    valid_count = max(1, int(len(rows) * valid_ratio)) if rows else 0
    valid_indices = set(indices[:valid_count])
    with train_path.open("w", encoding="utf-8") as train, valid_path.open("w", encoding="utf-8") as valid:
        for index, row in enumerate(rows):
            (valid if index in valid_indices else train).write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")
    print(f"Wrote {train_path}")
    print(f"Wrote {valid_path}")


if __name__ == "__main__":
    main()
