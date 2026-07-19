#!/usr/bin/env python3
"""Build compact one-note-in/one-note-out backing-vocal training examples.

The source is the existing phrase-window JSONL dataset. Unlike the LLM data,
the compact representation contains only musical features and one semitone
offset target per lead note. Train/validation assignment is deterministic per
song directory, so overlapping windows from one song cannot leak across the
split.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from collections import Counter
from pathlib import Path
from typing import Any

import yaml


SINGLE_PART_STYLES = (
    "UNISON",
    "OCT_UP",
    "OCT_DOWN",
    "THIRD_UP",
    "THIRD_DOWN",
    "SIXTH_UP",
    "SIXTH_DOWN",
    "FIFTH_UP",
    "FIFTH_DOWN",
    "FOURTH_UP",
    "FOURTH_DOWN",
    "DRONE_ROOT",
    "DRONE_FIFTH",
    "DRONE_THIRD",
    "CONTRARY",
    "OBLIQUE",
    "PAR3",
    "PAR6",
    "CHOIR_SOP",
    "CHOIR_ALT",
    "CHOIR_TEN",
    "CHOIR_BASS",
    "SUSP",
    "PASSING",
    "TENSION_RES",
)

NOTE_TO_PC = {
    "C": 0,
    "C#": 1,
    "DB": 1,
    "D": 2,
    "D#": 3,
    "EB": 3,
    "E": 4,
    "F": 5,
    "F#": 6,
    "GB": 6,
    "G": 7,
    "G#": 8,
    "AB": 8,
    "A": 9,
    "A#": 10,
    "BB": 10,
    "B": 11,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, required=True, help="Phrase-window train.jsonl")
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--max-per-style", type=int, default=1000)
    parser.add_argument("--valid-percent", type=int, default=10)
    parser.add_argument("--max-lines", type=int, default=0, help="0 scans until every style reaches its cap")
    parser.add_argument("--progress-every", type=int, default=10000)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    output_root = args.output_root.expanduser().resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    counts: Counter[str] = Counter()
    split_counts: Counter[str] = Counter()
    rejected: Counter[str] = Counter()
    scanned = 0
    train_cap = args.max_per_style
    valid_cap = max(1, round(args.max_per_style * args.valid_percent / max(1, 100 - args.valid_percent)))

    with args.input.expanduser().open(encoding="utf-8") as source, \
            (output_root / "train.jsonl").open("w", encoding="utf-8") as train, \
            (output_root / "valid.jsonl").open("w", encoding="utf-8") as valid:
        for line in source:
            scanned += 1
            if args.max_lines > 0 and scanned > args.max_lines:
                break
            try:
                raw = json.loads(line)
                style = str(raw.get("metadata", {}).get("style_id", ""))
                if style not in SINGLE_PART_STYLES:
                    continue
                group = song_group(str(raw["metadata"]["source_file"]))
                is_valid = stable_bucket(group) < args.valid_percent
                split_key = f"{'valid' if is_valid else 'train'}:{style}"
                split_cap = valid_cap if is_valid else train_cap
                if split_counts[split_key] >= split_cap:
                    continue
                example = compact_example(raw)
                if example is None:
                    rejected[style or "unknown"] += 1
                    continue
            except (KeyError, TypeError, ValueError, yaml.YAMLError):
                rejected["parse_error"] += 1
                continue
            handle = valid if is_valid else train
            handle.write(json.dumps(example, ensure_ascii=False, separators=(",", ":")) + "\n")
            counts[style] += 1
            split_counts[split_key] += 1

            if args.progress_every > 0 and scanned % args.progress_every == 0:
                print(json.dumps({"scanned": scanned, "accepted": sum(counts.values()), "styles_complete": sum(split_counts[f'train:{s}'] >= train_cap and split_counts[f'valid:{s}'] >= valid_cap for s in SINGLE_PART_STYLES)}), flush=True)
            if all(split_counts[f"train:{style}"] >= train_cap and split_counts[f"valid:{style}"] >= valid_cap for style in SINGLE_PART_STYLES):
                break

    manifest = {
        "schema": "synthetic-obsidian-backing-transformer-v1",
        "input": str(args.input.expanduser().resolve()),
        "scanned_lines": scanned,
        "examples": sum(counts.values()),
        "max_per_style": args.max_per_style,
        "train_cap_per_style": train_cap,
        "valid_cap_per_style": valid_cap,
        "valid_percent": args.valid_percent,
        "styles": list(SINGLE_PART_STYLES),
        "style_counts": dict(sorted(counts.items())),
        "split_counts": dict(sorted(split_counts.items())),
        "rejected": dict(sorted(rejected.items())),
    }
    (output_root / "manifest.json").write_text(json.dumps(manifest, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


def compact_example(raw: dict[str, Any]) -> dict[str, Any] | None:
    messages = raw["messages"]
    user_text = str(messages[1]["content"])
    yaml_start = user_text.find("meta:\n")
    if yaml_start < 0:
        return None
    loader = getattr(yaml, "CSafeLoader", yaml.SafeLoader)
    source = yaml.load(user_text[yaml_start:], Loader=loader)
    target = yaml.load(str(messages[2]["content"]), Loader=loader)
    meta = source.get("meta", {})
    tracks = source.get("tracks", {})
    lead = tracks.get("lead_vocal", [])
    chords = tracks.get("chords", [])
    backing_sets = target.get("tracks", {}).get("backing_vocals", [])
    if not isinstance(lead, list) or not lead or not isinstance(backing_sets, list) or len(backing_sets) != 1:
        return None
    parts = backing_sets[0].get("parts", [])
    if not isinstance(parts, list) or len(parts) != 1:
        return None

    targets = {}
    for note in parts[0].get("notes", []):
        source_ref = str(note.get("source_lead_ref", ""))
        if source_ref:
            targets[source_ref] = int(note["midi_note"])
    if any(str(note.get("id", "")) not in targets for note in lead):
        return None

    chord_by_id = {str(chord.get("id", "")): chord for chord in chords}
    meter = meter_numerator(str(meta.get("meter", "4/4")))
    key_pc, key_mode = parse_key(str(meta.get("key", "C major")))
    notes = []
    previous_pitch = int(lead[0]["midi_note"])
    previous_end = float(lead[0]["start"])
    previous_phrase = ""
    for index, note in enumerate(lead):
        pitch = int(note["midi_note"])
        start = float(note["start"])
        duration = float(note["duration"])
        phrase = str(note.get("phrase_id", ""))
        chord = chord_by_id.get(str(note.get("chord_ref", ""))) or chord_at(chords, start)
        root = note_pc(str(chord.get("root", ""))) if chord else 12
        chord_mask = chord_pitch_mask(chord)
        target_pitch = targets[str(note["id"])]
        offset = target_pitch - pitch
        if not -24 <= offset <= 24:
            return None
        beat = float(note.get("beat", 1.0))
        notes.append(
            {
                "pitch": pitch,
                "duration": round(duration, 5),
                "gap": round(max(0.0, start - previous_end), 5),
                "lead_delta": max(-24, min(24, pitch - previous_pitch)),
                "beat": round(beat, 5),
                "meter": meter,
                "velocity": int(note.get("velocity", 80)),
                "phrase_start": index == 0 or phrase != previous_phrase,
                "chord_root": root,
                "chord_quality": quality_id(str(chord.get("quality", "unknown"))) if chord else 0,
                "chord_mask": chord_mask,
                "target_offset": offset,
            }
        )
        previous_pitch = pitch
        previous_end = start + duration
        previous_phrase = phrase

    metadata = raw.get("metadata", {})
    return {
        "style": str(metadata["style_id"]),
        "key_pc": key_pc,
        "key_mode": key_mode,
        "notes": notes,
        "source_file": str(metadata.get("source_file", "")),
        "window_index": int(metadata.get("window_index", 0)),
    }


def chord_at(chords: list[dict[str, Any]], beat: float) -> dict[str, Any] | None:
    active = [chord for chord in chords if float(chord.get("start", 0.0)) <= beat < float(chord.get("start", 0.0)) + float(chord.get("duration", 0.0))]
    if active:
        return active[-1]
    return min(chords, key=lambda chord: abs(float(chord.get("start", 0.0)) - beat)) if chords else None


def chord_pitch_mask(chord: dict[str, Any] | None) -> int:
    mask = 0
    if chord:
        for note in chord.get("notes", []) or []:
            pc = note_pc(str(note))
            if pc < 12:
                mask |= 1 << pc
    return mask


def note_pc(note: str) -> int:
    match = re.match(r"^([A-Ga-g](?:#|b)?)", note.strip())
    if not match:
        return 12
    return NOTE_TO_PC.get(match.group(1).upper(), 12)


def parse_key(key: str) -> tuple[int, int]:
    pieces = key.split()
    pc = note_pc(pieces[0]) if pieces else 12
    lowered = key.lower()
    mode = 2 if "minor" in lowered else 1 if "major" in lowered else 0
    return pc, mode


def quality_id(quality: str) -> int:
    normalized = quality.lower().strip()
    qualities = {
        "unknown": 0, "major": 1, "minor": 2, "dominant": 3, "major7": 4,
        "minor7": 5, "diminished": 6, "half-diminished": 7, "augmented": 8,
        "sus2": 9, "sus4": 10, "sixth": 11, "minor6": 12,
    }
    if normalized in qualities:
        return qualities[normalized]
    if "minor" in normalized:
        return 2
    if "major" in normalized:
        return 1
    if "dim" in normalized:
        return 6
    if "sus2" in normalized:
        return 9
    if "sus" in normalized:
        return 10
    return 0


def meter_numerator(meter: str) -> int:
    try:
        return max(1, min(12, int(meter.split("/", 1)[0])))
    except ValueError:
        return 4


def song_group(source_file: str) -> str:
    return str(Path(source_file).parent)


def stable_bucket(value: str) -> int:
    return int.from_bytes(hashlib.sha256(value.encode("utf-8")).digest()[:4], "big") % 100


if __name__ == "__main__":
    main()
