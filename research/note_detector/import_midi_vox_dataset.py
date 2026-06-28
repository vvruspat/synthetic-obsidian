#!/usr/bin/env python3
"""Normalize local vocal+MIDI segment pairs into note-detector labels."""

from __future__ import annotations

import argparse
import hashlib
import json
import statistics
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
import pretty_midi
import soundfile as sf


@dataclass(frozen=True)
class MidiNote:
    start: float
    end: float
    midi: int
    velocity: int
    lyric: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Import local .wav + .mid segment pairs into normalized JSON/JSONL manifests."
    )
    parser.add_argument(
        "--input-dir",
        type=Path,
        required=True,
        help="Directory with matching basename .wav and .mid files.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("data/note_detector/local_segments"),
        help="Destination directory for labels, manifest, and summary.",
    )
    parser.add_argument(
        "--min-note-seconds",
        type=float,
        default=0.03,
        help="Drop MIDI notes shorter than this threshold.",
    )
    parser.add_argument(
        "--split-gap-seconds",
        type=float,
        default=0.35,
        help="Start a new chunk when the gap between notes exceeds this threshold.",
    )
    parser.add_argument(
        "--max-chunk-seconds",
        type=float,
        default=12.0,
        help="Maximum duration of a chunk before forced split.",
    )
    parser.add_argument(
        "--max-notes-per-chunk",
        type=int,
        default=48,
        help="Maximum notes in a single chunk before forced split.",
    )
    parser.add_argument(
        "--pre-roll-seconds",
        type=float,
        default=0.12,
        help="Audio padding added before the first chunk note.",
    )
    parser.add_argument(
        "--post-roll-seconds",
        type=float,
        default=0.18,
        help="Audio padding added after the last chunk note.",
    )
    parser.add_argument(
        "--audio-threshold-db",
        type=float,
        default=-42.0,
        help="Relative peak threshold for first detectable vocal activity.",
    )
    return parser.parse_args()


def wav_info(path: Path) -> tuple[int, int, float]:
    with wave.open(str(path), "rb") as handle:
        sample_rate = handle.getframerate()
        frames = handle.getnframes()
        duration = frames / sample_rate if sample_rate else 0.0
    return sample_rate, frames, duration


def first_audio_activity(path: Path, threshold_db: float, frame: int = 2048, hop: int = 256) -> float:
    audio, sample_rate = sf.read(str(path), always_2d=False)
    if getattr(audio, "ndim", 1) > 1:
        audio = audio.mean(axis=1)
    if len(audio) == 0:
        return 0.0
    peak = float(np.max(np.abs(audio)))
    if peak <= 1e-9:
        return 0.0

    threshold = peak * (10 ** (threshold_db / 20.0))
    limit = max(1, len(audio) - frame)
    for start in range(0, limit, hop):
        if float(np.max(np.abs(audio[start : start + frame]))) >= threshold:
            return start / sample_rate
    return 0.0


def load_notes(path: Path, min_note_seconds: float) -> list[MidiNote]:
    midi = pretty_midi.PrettyMIDI(str(path))
    notes = sorted((note for instrument in midi.instruments for note in instrument.notes), key=lambda n: n.start)
    output: list[MidiNote] = []
    for note in notes:
        duration = note.end - note.start
        if duration < min_note_seconds:
            continue
        output.append(
            MidiNote(
                start=float(note.start),
                end=float(note.end),
                midi=int(note.pitch),
                velocity=int(note.velocity),
                lyric="",
            )
        )
    return output


def deterministic_split(basename: str) -> str:
    bucket = int(hashlib.md5(basename.encode("utf-8")).hexdigest()[:8], 16) % 100
    if bucket < 10:
        return "test"
    if bucket < 20:
        return "val"
    return "train"


def split_note_chunks(notes: list[MidiNote], split_gap_seconds: float, max_chunk_seconds: float, max_notes_per_chunk: int) -> list[list[MidiNote]]:
    if not notes:
        return []

    chunks: list[list[MidiNote]] = []
    current: list[MidiNote] = [notes[0]]
    current_start = notes[0].start

    for prev, note in zip(notes, notes[1:]):
        gap = max(0.0, note.start - prev.end)
        chunk_duration = note.end - current_start
        should_split = (
            gap >= split_gap_seconds
            or chunk_duration > max_chunk_seconds
            or len(current) >= max_notes_per_chunk
        )
        if should_split:
            chunks.append(current)
            current = [note]
            current_start = note.start
            continue
        current.append(note)

    if current:
        chunks.append(current)
    return chunks


def build_label_payload(
    basename: str,
    wav_path: Path,
    midi_path: Path,
    notes: list[MidiNote],
    sample_rate: int,
    frame_count: int,
    duration: float,
    audio_activity_start: float,
) -> dict[str, Any]:
    first_midi_start = notes[0].start if notes else 0.0
    last_midi_end = notes[-1].end if notes else 0.0
    return {
        "schema": "synthetic-obsidian.note-detector.local-midi-vox.v1",
        "source": "local_midi_vox",
        "id": basename,
        "audio": str(wav_path),
        "midi": str(midi_path),
        "split": deterministic_split(basename),
        "audio_info": {
            "sample_rate": sample_rate,
            "frame_count": frame_count,
            "duration_seconds": duration,
            "channels": sf.info(str(wav_path)).channels,
        },
        "alignment": {
            "audio_activity_start": audio_activity_start,
            "first_midi_note_start": first_midi_start,
            "last_midi_note_end": last_midi_end,
            "midi_minus_audio_activity": first_midi_start - audio_activity_start,
        },
        "notes": [note.__dict__ for note in notes],
    }


def build_manifest_rows(
    basename: str,
    label_path: Path,
    wav_path: Path,
    notes: list[MidiNote],
    duration: float,
    split_gap_seconds: float,
    max_chunk_seconds: float,
    max_notes_per_chunk: int,
    pre_roll_seconds: float,
    post_roll_seconds: float,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for index, chunk in enumerate(
        split_note_chunks(notes, split_gap_seconds, max_chunk_seconds, max_notes_per_chunk)
    ):
        chunk_start = max(0.0, chunk[0].start - pre_roll_seconds)
        chunk_end = min(duration, chunk[-1].end + post_roll_seconds)
        rows.append(
            {
                "id": f"{basename}_chunk_{index:03d}",
                "source_id": basename,
                "audio": str(wav_path),
                "label": str(label_path),
                "split": deterministic_split(basename),
                "start": chunk_start,
                "end": chunk_end,
                "duration_seconds": chunk_end - chunk_start,
                "note_count": len(chunk),
                "midi_start": chunk[0].start,
                "midi_end": chunk[-1].end,
                "notes_range": [notes.index(chunk[0]), notes.index(chunk[-1])],
            }
        )
    return rows


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def pair_basenames(input_dir: Path) -> tuple[list[str], list[str], list[str]]:
    wav_names = {path.stem for path in input_dir.glob("*.wav")}
    midi_names = {path.stem for path in input_dir.glob("*.mid")}
    paired = sorted(wav_names & midi_names)
    missing_wav = sorted(midi_names - wav_names)
    missing_midi = sorted(wav_names - midi_names)
    return paired, missing_wav, missing_midi


def main() -> int:
    args = parse_args()
    input_dir = args.input_dir.resolve()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    paired, missing_wav, missing_midi = pair_basenames(input_dir)
    manifest_rows: list[dict[str, Any]] = []
    warnings: list[str] = []
    label_dir = output_dir / "labels"

    audio_durations: list[float] = []
    note_counts: list[int] = []
    chunk_durations: list[float] = []
    offsets: list[float] = []

    for basename in paired:
        wav_path = input_dir / f"{basename}.wav"
        midi_path = input_dir / f"{basename}.mid"

        try:
            notes = load_notes(midi_path, args.min_note_seconds)
            sample_rate, frame_count, duration = wav_info(wav_path)
            audio_activity_start = first_audio_activity(wav_path, args.audio_threshold_db)
        except Exception as exc:
            warnings.append(f"{basename}: failed to read pair ({exc})")
            continue

        if not notes:
            warnings.append(f"{basename}: no usable MIDI notes")
            continue

        label_payload = build_label_payload(
            basename=basename,
            wav_path=wav_path,
            midi_path=midi_path,
            notes=notes,
            sample_rate=sample_rate,
            frame_count=frame_count,
            duration=duration,
            audio_activity_start=audio_activity_start,
        )
        label_path = label_dir / f"{basename}.json"
        write_json(label_path, label_payload)

        rows = build_manifest_rows(
            basename=basename,
            label_path=label_path,
            wav_path=wav_path,
            notes=notes,
            duration=duration,
            split_gap_seconds=args.split_gap_seconds,
            max_chunk_seconds=args.max_chunk_seconds,
            max_notes_per_chunk=args.max_notes_per_chunk,
            pre_roll_seconds=args.pre_roll_seconds,
            post_roll_seconds=args.post_roll_seconds,
        )
        manifest_rows.extend(rows)

        audio_durations.append(duration)
        note_counts.append(len(notes))
        offsets.append(label_payload["alignment"]["midi_minus_audio_activity"])
        chunk_durations.extend(row["duration_seconds"] for row in rows)

    manifest_path = output_dir / "manifest.jsonl"
    with manifest_path.open("w", encoding="utf-8") as handle:
        for row in manifest_rows:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")

    summary = {
        "schema": "synthetic-obsidian.note-detector.import-summary.v1",
        "input_dir": str(input_dir),
        "paired_files": len(paired),
        "missing_wav": missing_wav,
        "missing_midi": missing_midi,
        "imported_labels": len(note_counts),
        "manifest_rows": len(manifest_rows),
        "audio_minutes": sum(audio_durations) / 60.0 if audio_durations else 0.0,
        "note_count_stats": stats_dict(note_counts),
        "audio_duration_stats": stats_dict(audio_durations),
        "chunk_duration_stats": stats_dict(chunk_durations),
        "offset_stats": stats_dict(offsets),
        "warnings_count": len(warnings),
    }
    write_json(output_dir / "summary.json", summary)

    if warnings:
        (output_dir / "warnings.txt").write_text("\n".join(warnings) + "\n", encoding="utf-8")

    print(f"Paired files: {len(paired)}")
    print(f"Imported labels: {len(note_counts)}")
    print(f"Manifest rows: {len(manifest_rows)}")
    print(f"Output: {output_dir}")
    if warnings:
        print(f"Warnings: {len(warnings)} ({output_dir / 'warnings.txt'})")
    return 0


def stats_dict(values: list[float] | list[int]) -> dict[str, float] | None:
    if not values:
        return None
    ordered = sorted(float(v) for v in values)
    return {
        "min": ordered[0],
        "median": statistics.median(ordered),
        "mean": statistics.mean(ordered),
        "p90": ordered[min(len(ordered) - 1, int(0.9 * (len(ordered) - 1)))],
        "max": ordered[-1],
    }


if __name__ == "__main__":
    raise SystemExit(main())
