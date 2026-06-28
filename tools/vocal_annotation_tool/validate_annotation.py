#!/usr/bin/env python3
"""Validate Synthetic Obsidian vocal annotation JSON files."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
from typing import Any

BOUNDARY_KINDS = {"syllable", "rearticulation", "legato", "breath", "noise", "pause", "ignore"}
REGION_KINDS = {"slide_in", "slide_out", "drift", "vibrato", "melisma", "breath", "noise", "pause"}


def _number(value: Any) -> bool:
    return isinstance(value, (int, float)) and math.isfinite(float(value))


def validate(path: Path) -> list[str]:
    errors: list[str] = []
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except Exception as exc:  # noqa: BLE001 - CLI should report parse failures.
        return [f"Cannot read JSON: {exc}"]

    if not isinstance(data, dict):
        return ["Top-level JSON value must be an object"]

    for key in ("version", "audio", "sample_rate", "duration", "bpm", "key", "notes", "boundaries", "regions"):
        if key not in data:
            errors.append(f"Missing top-level field: {key}")

    duration = float(data.get("duration", 0.0)) if _number(data.get("duration")) else 0.0
    if duration <= 0.0:
        errors.append("duration must be a positive finite number")

    audio = data.get("audio")
    if isinstance(audio, str) and audio:
        audio_path = Path(audio)
        if not audio_path.is_absolute():
            audio_path = path.parent / audio_path
        if not audio_path.exists():
            errors.append(f"audio path does not exist: {audio}")
    else:
        errors.append("audio must be a non-empty string")

    notes = data.get("notes", [])
    if not isinstance(notes, list):
        errors.append("notes must be an array")
        notes = []

    note_ids: set[str] = set()
    spans: list[tuple[float, float, str, set[str]]] = []
    for index, note in enumerate(notes):
        if not isinstance(note, dict):
            errors.append(f"notes[{index}] must be an object")
            continue
        note_id = str(note.get("id", f"notes[{index}]"))
        note_ids.add(note_id)
        start = note.get("start")
        end = note.get("end")
        pitch = note.get("pitch")
        if not _number(start) or not _number(end) or float(end) <= float(start):
            errors.append(f"{note_id}: note must satisfy finite end > start")
            continue
        start_f = float(start)
        end_f = float(end)
        flags_value = note.get("flags", [])
        flags = {str(flag) for flag in flags_value} if isinstance(flags_value, list) else set()
        spans.append((start_f, end_f, note_id, flags))
        if start_f < 0.0 or end_f > duration:
            errors.append(f"{note_id}: note is outside clip duration")
        if not isinstance(pitch, int) or pitch < 0 or pitch > 127:
            errors.append(f"{note_id}: pitch must be an integer MIDI note 0..127")
        pitch_exact = note.get("pitch_exact", float(pitch) if isinstance(pitch, int) else None)
        if not _number(pitch_exact) or float(pitch_exact) < 0.0 or float(pitch_exact) > 127.0:
            errors.append(f"{note_id}: pitch_exact must be a finite MIDI value 0..127")
        voiced_start = note.get("voiced_start", start_f)
        voiced_end = note.get("voiced_end", end_f)
        if not _number(voiced_start) or not _number(voiced_end) or float(voiced_end) < float(voiced_start):
            errors.append(f"{note_id}: invalid voiced_start/voiced_end")
        curve = note.get("curve", [])
        if not isinstance(curve, list):
            errors.append(f"{note_id}: curve must be an array")
            continue
        last_time = -math.inf
        for point_index, point in enumerate(curve):
            if not isinstance(point, dict):
                errors.append(f"{note_id}: curve[{point_index}] must be an object")
                continue
            time = point.get("time")
            midi = point.get("midi")
            confidence = point.get("confidence", 1.0)
            if not _number(time) or not _number(midi) or not _number(confidence):
                errors.append(f"{note_id}: curve[{point_index}] has non-finite values")
                continue
            time_f = float(time)
            if time_f < last_time:
                errors.append(f"{note_id}: curve points must be sorted by time")
            last_time = time_f
            if time_f < 0.0 or time_f > duration:
                errors.append(f"{note_id}: curve[{point_index}] is outside clip duration")
            if float(confidence) < 0.0 or float(confidence) > 1.0:
                errors.append(f"{note_id}: curve[{point_index}] confidence must be 0..1")

    for left, right in zip(sorted(spans), sorted(spans)[1:]):
        if left[1] > right[0] and "allow_overlap" not in left[3] and "allow_overlap" not in right[3]:
            errors.append(f"overlapping notes without allow_overlap flag: {left[2]} and {right[2]}")

    boundaries = data.get("boundaries", [])
    if not isinstance(boundaries, list):
        errors.append("boundaries must be an array")
        boundaries = []
    for index, boundary in enumerate(boundaries):
        if not isinstance(boundary, dict):
            errors.append(f"boundaries[{index}] must be an object")
            continue
        boundary_id = str(boundary.get("id", f"boundaries[{index}]"))
        time = boundary.get("time")
        kind = boundary.get("kind")
        if not _number(time) or float(time) < 0.0 or float(time) > duration:
            errors.append(f"{boundary_id}: time is outside clip duration")
        if kind not in BOUNDARY_KINDS:
            errors.append(f"{boundary_id}: invalid boundary kind: {kind}")

    regions = data.get("regions", [])
    if not isinstance(regions, list):
        errors.append("regions must be an array")
        regions = []
    for index, region in enumerate(regions):
        if not isinstance(region, dict):
            errors.append(f"regions[{index}] must be an object")
            continue
        region_id = f"regions[{index}]"
        start = region.get("start")
        end = region.get("end")
        kind = region.get("kind")
        if not _number(start) or not _number(end) or float(end) <= float(start):
            errors.append(f"{region_id}: region must satisfy finite end > start")
        elif float(start) < 0.0 or float(end) > duration:
            errors.append(f"{region_id}: region is outside clip duration")
        if kind not in REGION_KINDS:
            errors.append(f"{region_id}: invalid region kind: {kind}")
        note_id = region.get("note_id")
        if note_id and str(note_id) not in note_ids:
            errors.append(f"{region_id}: note_id does not reference an existing note: {note_id}")

    return errors


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("annotation", type=Path)
    args = parser.parse_args()

    errors = validate(args.annotation)
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1

    print("OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
