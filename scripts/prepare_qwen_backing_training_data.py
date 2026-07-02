#!/usr/bin/env python3
"""Prepare two-stage Qwen backing-vocal training JSONL files."""

from __future__ import annotations

import argparse
import json
import random
from pathlib import Path


THEORY_ORDER = (
    "interval_tasks.jsonl",
    "chord_degree_tasks.jsonl",
    "motion_type_tasks.jsonl",
    "voice_leading_analysis.jsonl",
    "preference_pairs.jsonl",
    "repair_tasks.jsonl",
    "theory_mix_seed.jsonl",
)

SYSTEM_DSL = (
    "You generate Synthetic Obsidian backing-vocal YAML DSL. "
    "Return only valid YAML for tracks.backing_vocals. "
    "Do not include analysis, chords, lead_vocal, markdown, or explanations."
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--theory-root", type=Path, required=True)
    parser.add_argument("--dsl-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--stage2-song-limit", type=int, default=0, help="0 means all songs.")
    parser.add_argument("--stage2-max-lead-notes", type=int, default=220)
    parser.add_argument("--stage2-max-chords", type=int, default=300)
    parser.add_argument("--valid-ratio", type=float, default=0.05)
    parser.add_argument("--seed", type=int, default=42)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    output_root = args.output_root.expanduser().resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    rng = random.Random(args.seed)

    stage1 = load_stage1(args.theory_root.expanduser().resolve())
    write_split(stage1, output_root / "stage1_theory_train.jsonl", output_root / "stage1_theory_valid.jsonl", args.valid_ratio, rng)

    stage2 = build_stage2(
        args.dsl_root.expanduser().resolve(),
        args.stage2_song_limit,
        args.stage2_max_lead_notes,
        args.stage2_max_chords,
    )
    write_split(stage2, output_root / "stage2_dsl_train.jsonl", output_root / "stage2_dsl_valid.jsonl", args.valid_ratio, rng)

    manifest = {
        "stage1_examples": len(stage1),
        "stage2_examples": len(stage2),
        "stage2_song_limit": args.stage2_song_limit,
        "stage2_max_lead_notes": args.stage2_max_lead_notes,
        "stage2_max_chords": args.stage2_max_chords,
        "valid_ratio": args.valid_ratio,
    }
    (output_root / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps(manifest, indent=2))


def load_stage1(theory_root: Path) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    for name in THEORY_ORDER:
        path = theory_root / "data" / name
        with path.open(encoding="utf-8") as handle:
            for line in handle:
                if line.strip():
                    rows.append(json.loads(line))
    return rows


def build_stage2(dsl_root: Path, song_limit: int, max_lead_notes: int, max_chords: int) -> list[dict[str, object]]:
    rows: list[dict[str, object]] = []
    files = sorted(dsl_root.rglob("*.yaml"))
    if song_limit > 0:
        files = files[:song_limit]
    for song_index, path in enumerate(files, start=1):
        text = path.read_text(encoding="utf-8")
        source, lead_refs = extract_source_context(text, max_lead_notes, max_chords)
        for backing in extract_backing_sets(text):
            block_lines = filter_backing_to_lead_refs(backing["block_lines"], lead_refs)
            if not block_lines:
                continue
            style_id = backing.get("id", "")
            style_name = backing.get("name", "")
            user = (
                f"TASK GENERATE_BACKING_VOCAL\n"
                f"REQUESTED_STYLE_ID: {style_id}\n"
                f"REQUESTED_STYLE_NAME: {style_name}\n"
                f"SOURCE_FILE: {path.relative_to(dsl_root).as_posix()}\n\n"
                f"{source}"
            )
            assistant = "tracks:\n" + render_block("  backing_vocals:", block_lines)
            rows.append(
                {
                    "messages": [
                        {"role": "system", "content": SYSTEM_DSL},
                        {"role": "user", "content": user},
                        {"role": "assistant", "content": assistant.rstrip() + "\n"},
                    ],
                    "metadata": {
                        "task": "generate_backing_vocal",
                        "source_file": path.relative_to(dsl_root).as_posix(),
                        "style_id": style_id,
                        "style_name": style_name,
                        "song_index": song_index,
                    },
                }
            )
    return rows


def extract_source_context(text: str, max_lead_notes: int, max_chords: int) -> tuple[str, set[str]]:
    lines = text.splitlines()
    meta = extract_top_level_block(lines, "meta:")
    chords = limit_yaml_list(extract_tracks_subblock(lines, "chords:"), max_chords)
    lead = limit_yaml_list(extract_tracks_subblock(lines, "lead_vocal:"), max_lead_notes)
    lead_refs = {line.strip().removeprefix("- id: ").strip() for line in lead if line.startswith("    - id: ")}
    source = "\n".join(
        [
            "meta:",
            *meta,
            "tracks:",
            "  chords:",
            *chords,
            "  lead_vocal:",
            *lead,
        ]
    ).rstrip() + "\n"
    return source, lead_refs


def extract_top_level_block(lines: list[str], header: str) -> list[str]:
    output: list[str] = []
    active = False
    for line in lines:
        if line == header:
            active = True
            continue
        if active and line and not line.startswith(" "):
            break
        if active:
            output.append(line)
    return output


def extract_tracks_subblock(lines: list[str], header: str) -> list[str]:
    output: list[str] = []
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
            output.append(line)
    return output


def limit_yaml_list(lines: list[str], limit: int) -> list[str]:
    if limit <= 0:
        return lines
    output: list[str] = []
    count = 0
    for line in lines:
        if line.startswith("    - "):
            count += 1
            if count > limit:
                break
        output.append(line)
    return output


def extract_backing_sets(text: str) -> list[dict[str, object]]:
    lines = text.splitlines()
    backing_lines = extract_tracks_subblock(lines, "backing_vocals:")
    sets: list[dict[str, object]] = []
    current: list[str] = []
    for line in backing_lines:
        if line.startswith("    - ") and current:
            sets.append(parse_backing_set(current))
            current = [line]
        elif line.startswith("    - "):
            current = [line]
        elif current:
            current.append(line)
    if current:
        sets.append(parse_backing_set(current))
    return sets


def parse_backing_set(lines: list[str]) -> dict[str, object]:
    style_id = ""
    style_name = ""
    for line in lines:
        if line.startswith("    - id: "):
            style_id = line.strip().removeprefix("- id: ").strip().strip('"')
        elif line.startswith("      name: "):
            style_name = line.strip().removeprefix("name: ").strip().strip('"')
    return {"id": style_id, "name": style_name, "block_lines": lines}


def filter_backing_to_lead_refs(block_lines: object, lead_refs: set[str]) -> list[str]:
    assert isinstance(block_lines, list)
    lines = [str(line) for line in block_lines]
    header: list[str] = []
    part_blocks: list[list[str]] = []
    current_part: list[str] | None = None
    for line in lines:
        if line.startswith("        - id: "):
            if current_part is not None:
                part_blocks.append(current_part)
            current_part = [line]
        elif current_part is None:
            header.append(line)
        else:
            current_part.append(line)
    if current_part is not None:
        part_blocks.append(current_part)

    filtered_parts: list[list[str]] = []
    for part in part_blocks:
        filtered = filter_part_to_lead_refs(part, lead_refs)
        if filtered:
            filtered_parts.append(filtered)
    if not filtered_parts:
        return []
    output = header
    for part in filtered_parts:
        output.extend(part)
    return output


def filter_part_to_lead_refs(part_lines: list[str], lead_refs: set[str]) -> list[str]:
    before_notes: list[str] = []
    note_blocks: list[list[str]] = []
    current_note: list[str] | None = None
    in_notes = False
    for line in part_lines:
        if line.startswith("          notes:"):
            in_notes = True
            before_notes.append(line)
            continue
        if in_notes and line.startswith("            - id: "):
            if current_note is not None:
                note_blocks.append(current_note)
            current_note = [line]
        elif current_note is not None:
            current_note.append(line)
        else:
            before_notes.append(line)
    if current_note is not None:
        note_blocks.append(current_note)

    kept_notes = [note for note in note_blocks if note_source_ref(note) in lead_refs]
    if not kept_notes:
        return []
    output = before_notes
    for index, note in enumerate(kept_notes, start=1):
        output.extend(rename_note(note, index))
    return output


def note_source_ref(note_lines: list[str]) -> str:
    for line in note_lines:
        stripped = line.strip()
        if stripped.startswith("source_lead_ref: "):
            return stripped.removeprefix("source_lead_ref: ").strip()
    return ""


def rename_note(note_lines: list[str], index: int) -> list[str]:
    if not note_lines:
        return note_lines
    first = note_lines[0]
    prefix, value = first.split(": ", 1)
    stem = value.rsplit("_", 1)[0]
    return [f"{prefix}: {stem}_{index:03d}", *note_lines[1:]]


def render_block(header: str, block_lines: object) -> str:
    lines = [header]
    assert isinstance(block_lines, list)
    lines.extend(str(line) for line in block_lines)
    return "\n".join(lines) + "\n"


def write_split(rows: list[dict[str, object]], train_path: Path, valid_path: Path, valid_ratio: float, rng: random.Random) -> None:
    indices = list(range(len(rows)))
    rng.shuffle(indices)
    valid_count = max(1, int(len(rows) * valid_ratio)) if rows else 0
    valid_indices = set(indices[:valid_count])
    with train_path.open("w", encoding="utf-8") as train, valid_path.open("w", encoding="utf-8") as valid:
        for index, row in enumerate(rows):
            handle = valid if index in valid_indices else train
            handle.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")
    print(f"Wrote {train_path}")
    print(f"Wrote {valid_path}")


if __name__ == "__main__":
    main()
