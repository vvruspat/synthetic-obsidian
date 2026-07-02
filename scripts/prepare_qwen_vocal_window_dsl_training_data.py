#!/usr/bin/env python3
"""Prepare vocal-window YAML DSL chat data for Qwen LoRA training.

The output keeps the native Synthetic Obsidian YAML shape. Each row contains a
small vocal window from a one-style DSL file and asks the model to generate that
style's `tracks.backing_vocals` YAML for the same window.
"""

from __future__ import annotations

import argparse
import json
import random
from dataclasses import dataclass
from pathlib import Path
from typing import TextIO


SYSTEM = (
    "You generate Synthetic Obsidian backing-vocal YAML DSL. "
    "Return only valid YAML containing tracks.backing_vocals. "
    "Do not include markdown, analysis, chords, lead_vocal, or explanations."
)


@dataclass(frozen=True)
class ItemBlock:
    id: str
    start: float
    end: float
    phrase_id: str
    lines: list[str]


@dataclass(frozen=True)
class Window:
    start: float
    end: float
    lead: list[ItemBlock]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=0, help="Maximum style YAML files to read. 0 means all.")
    parser.add_argument("--max-lead-notes", type=int, default=12)
    parser.add_argument("--min-lead-notes", type=int, default=1)
    parser.add_argument("--max-gap-beats", type=float, default=2.0)
    parser.add_argument("--chord-padding-beats", type=float, default=2.0)
    parser.add_argument("--valid-ratio", type=float, default=0.05)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--stream", action="store_true", help="Write rows while scanning to keep memory bounded.")
    parser.add_argument("--progress-every", type=int, default=1000)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    input_root = args.input_root.expanduser().resolve()
    output_root = args.output_root.expanduser().resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    files = sorted(input_root.rglob("*.yaml"))
    if args.limit > 0:
        files = files[: args.limit]

    if args.stream:
        train_count, valid_count = write_streaming(files, input_root, output_root, args)
        total_examples = train_count + valid_count
    else:
        rows: list[dict[str, object]] = []
        for file_index, path in enumerate(files, start=1):
            rows.extend(build_rows_for_file(path, input_root, args, file_index))
        write_split(rows, output_root / "train.jsonl", output_root / "valid.jsonl", args.valid_ratio, random.Random(args.seed))
        total_examples = len(rows)
        train_count = 0
        valid_count = 0
    manifest = {
        "examples": total_examples,
        "train_examples": train_count,
        "valid_examples": valid_count,
        "source_files": len(files),
        "limit": args.limit,
        "max_lead_notes": args.max_lead_notes,
        "min_lead_notes": args.min_lead_notes,
        "max_gap_beats": args.max_gap_beats,
        "chord_padding_beats": args.chord_padding_beats,
        "stream": args.stream,
    }
    (output_root / "manifest.json").write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    print(json.dumps(manifest, indent=2))


def write_streaming(files: list[Path], input_root: Path, output_root: Path, args: argparse.Namespace) -> tuple[int, int]:
    train_count = 0
    valid_count = 0
    rng = random.Random(args.seed)
    with (output_root / "train.jsonl").open("w", encoding="utf-8") as train, (output_root / "valid.jsonl").open("w", encoding="utf-8") as valid:
        for file_index, path in enumerate(files, start=1):
            rows = build_rows_for_file(path, input_root, args, file_index)
            for row in rows:
                if rng.random() < args.valid_ratio:
                    write_row(valid, row)
                    valid_count += 1
                else:
                    write_row(train, row)
                    train_count += 1
            if args.progress_every > 0 and file_index % args.progress_every == 0:
                print(
                    json.dumps(
                        {
                            "processed_files": file_index,
                            "source_files": len(files),
                            "train_examples": train_count,
                            "valid_examples": valid_count,
                        },
                        ensure_ascii=False,
                    ),
                    flush=True,
                )
    return train_count, valid_count


def build_rows_for_file(path: Path, input_root: Path, args: argparse.Namespace, file_index: int) -> list[dict[str, object]]:
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    meta = extract_top_level_block(lines, "meta:")
    chords = parse_item_blocks(extract_tracks_subblock(lines, "chords:"))
    lead = parse_item_blocks(extract_tracks_subblock(lines, "lead_vocal:"))
    backing_sets = extract_backing_sets(lines)
    if not meta or not chords or not lead or len(backing_sets) != 1:
        return []

    backing = backing_sets[0]
    windows = build_vocal_windows(lead, args.max_lead_notes, args.min_lead_notes, args.max_gap_beats)
    rows: list[dict[str, object]] = []
    for window_index, window in enumerate(windows, start=1):
        lead_refs = {note.id for note in window.lead}
        chord_lines = select_chord_lines(chords, window.start - args.chord_padding_beats, window.end + args.chord_padding_beats)
        lead_lines = flatten_blocks(window.lead)
        backing_lines = filter_backing_to_lead_refs(backing["block_lines"], lead_refs)
        if not chord_lines or not lead_lines or not backing_lines:
            continue
        source = render_source_yaml(meta, chord_lines, lead_lines)
        style_id = str(backing.get("id", ""))
        style_name = str(backing.get("name", ""))
        user = (
            "TASK: GENERATE_BACKING_VOCAL\n"
            f"REQUESTED_STYLE_ID: {style_id}\n"
            f"REQUESTED_STYLE_NAME: {style_name}\n"
            f"SOURCE_FILE: {path.relative_to(input_root).as_posix()}\n"
            f"WINDOW_INDEX: {window_index}\n"
            f"WINDOW_START_BEAT: {window.start:g}\n"
            f"WINDOW_END_BEAT: {window.end:g}\n\n"
            f"{source}"
        )
        assistant = "tracks:\n" + render_block("  backing_vocals:", backing_lines)
        rows.append(
            {
                "messages": [
                    {"role": "system", "content": SYSTEM},
                    {"role": "user", "content": user},
                    {"role": "assistant", "content": assistant.rstrip() + "\n"},
                ],
                "metadata": {
                    "task": "generate_backing_vocal_yaml_window",
                    "source_file": path.relative_to(input_root).as_posix(),
                    "style_id": style_id,
                    "style_name": style_name,
                    "file_index": file_index,
                    "window_index": window_index,
                    "lead_notes": len(window.lead),
                },
            }
        )
    return rows


def build_vocal_windows(lead: list[ItemBlock], max_lead_notes: int, min_lead_notes: int, max_gap: float) -> list[Window]:
    if not lead:
        return []
    phrases: list[list[ItemBlock]] = []
    current: list[ItemBlock] = []
    for note in lead:
        previous = current[-1] if current else None
        phrase_changed = bool(previous and note.phrase_id and previous.phrase_id and note.phrase_id != previous.phrase_id)
        gap_changed = bool(previous and note.start - previous.end > max_gap)
        if current and (phrase_changed or gap_changed or len(current) >= max_lead_notes):
            phrases.append(current)
            current = []
        current.append(note)
    if current:
        phrases.append(current)

    windows: list[Window] = []
    for phrase in phrases:
        for index in range(0, len(phrase), max_lead_notes):
            chunk = phrase[index : index + max_lead_notes]
            if len(chunk) < min_lead_notes:
                continue
            windows.append(Window(start=chunk[0].start, end=max(note.end for note in chunk), lead=chunk))
    return windows


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


def parse_item_blocks(lines: list[str]) -> list[ItemBlock]:
    blocks: list[list[str]] = []
    current: list[str] = []
    for line in lines:
        if line.startswith("    - "):
            if current:
                blocks.append(current)
            current = [line]
        elif current:
            current.append(line)
    if current:
        blocks.append(current)
    return [parse_item_block(block) for block in blocks]


def parse_item_block(lines: list[str]) -> ItemBlock:
    values: dict[str, str] = {}
    for line in lines:
        stripped = line.strip()
        if ": " in stripped:
            key, value = stripped.removeprefix("- ").split(": ", 1) if stripped.startswith("- ") else stripped.split(": ", 1)
            values[key] = value.strip().strip('"')
    start = float(values.get("start", "0"))
    duration = float(values.get("duration", "0"))
    return ItemBlock(
        id=values.get("id", ""),
        start=start,
        end=start + duration,
        phrase_id=values.get("phrase_id", ""),
        lines=lines,
    )


def select_chord_lines(chords: list[ItemBlock], start: float, end: float) -> list[str]:
    selected = [chord for chord in chords if chord.end > start and chord.start < end]
    if not selected:
        selected = [min(chords, key=lambda chord: abs(chord.start - start))]
    return flatten_blocks(selected)


def flatten_blocks(blocks: list[ItemBlock]) -> list[str]:
    output: list[str] = []
    for block in blocks:
        output.extend(block.lines)
    return output


def extract_backing_sets(lines: list[str]) -> list[dict[str, object]]:
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
        stripped = line.strip()
        if line.startswith("    - id: "):
            style_id = stripped.removeprefix("- id: ").strip().strip('"')
        elif line.startswith("      name: "):
            style_name = stripped.removeprefix("name: ").strip().strip('"')
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


def render_source_yaml(meta: list[str], chord_lines: list[str], lead_lines: list[str]) -> str:
    return "\n".join(
        [
            "meta:",
            *meta,
            "tracks:",
            "  chords:",
            *chord_lines,
            "  lead_vocal:",
            *lead_lines,
        ]
    ).rstrip() + "\n"


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
            write_row(handle, row)
    print(f"Wrote {train_path}")
    print(f"Wrote {valid_path}")


def write_row(handle: TextIO, row: dict[str, object]) -> None:
    handle.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main()
