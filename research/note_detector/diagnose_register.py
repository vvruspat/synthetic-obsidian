#!/usr/bin/env python3
"""Diagnose register / octave mismatch between reference MIDI and vocal audio."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any

import librosa
import numpy as np
import pretty_midi


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Estimate audio-vs-MIDI register mismatch for a phrase.")
    parser.add_argument("--audio", type=Path, required=True)
    parser.add_argument("--midi", type=Path, required=True)
    parser.add_argument("--start", type=float, default=0.0)
    parser.add_argument("--end", type=float, default=0.0, help="0 means full audio file.")
    parser.add_argument("--midi-shift-seconds", type=float, default=0.0)
    parser.add_argument("--sample-rate", type=int, default=24000)
    parser.add_argument("--hop-length", type=int, default=256)
    parser.add_argument("--output-json", type=Path, default=None)
    return parser.parse_args()


def summarize(values: np.ndarray) -> dict[str, float] | None:
    if values.size == 0:
        return None
    return {
        "min": float(np.min(values)),
        "median": float(np.median(values)),
        "mean": float(np.mean(values)),
        "max": float(np.max(values)),
    }


def load_audio_midi(audio_path: Path, start: float, end: float, sample_rate: int, hop_length: int) -> np.ndarray:
    duration = None if end <= 0.0 else max(0.0, end - start)
    audio, sr = librosa.load(str(audio_path), sr=sample_rate, mono=True, offset=start, duration=duration)
    f0, voiced_flag, _ = librosa.pyin(
        audio,
        sr=sr,
        hop_length=hop_length,
        fmin=librosa.note_to_hz("C2"),
        fmax=librosa.note_to_hz("C7"),
        frame_length=2048,
    )
    audio_midi = librosa.hz_to_midi(f0)
    audio_midi = np.asarray(audio_midi, dtype=np.float32)
    audio_midi[~np.isfinite(audio_midi)] = np.nan
    if voiced_flag is not None:
        audio_midi[~np.asarray(voiced_flag, dtype=bool)] = np.nan
    return audio_midi[np.isfinite(audio_midi)]


def load_reference_pitches(midi_path: Path, start: float, end: float, midi_shift_seconds: float) -> np.ndarray:
    midi = pretty_midi.PrettyMIDI(str(midi_path))
    notes = sorted((note for inst in midi.instruments for note in inst.notes), key=lambda note: note.start)
    output: list[float] = []
    for note in notes:
        shifted_start = float(note.start) - midi_shift_seconds
        shifted_end = float(note.end) - midi_shift_seconds
        if shifted_end < start or (end > 0.0 and shifted_start > end):
            continue
        output.append(float(note.pitch))
    return np.asarray(output, dtype=np.float32)


def octave_normalized_delta(audio_stats: dict[str, float] | None, midi_stats: dict[str, float] | None) -> float | None:
    if audio_stats is None or midi_stats is None:
        return None
    delta = float(audio_stats["median"] - midi_stats["median"])
    return float(delta - 12.0 * round(delta / 12.0))


def likely_register_issue(raw_delta: float | None, octave_delta: float | None) -> str:
    if raw_delta is None or octave_delta is None:
        return "unknown"
    if abs(raw_delta) >= 10.5 and abs(octave_delta) <= 2.0:
        octaves = int(round(raw_delta / 12.0))
        return f"likely_{abs(octaves)}_octave_mismatch"
    if abs(raw_delta) >= 4.0:
        return "likely_register_mismatch"
    return "no_major_register_issue"


def main() -> int:
    args = parse_args()
    audio_values = load_audio_midi(args.audio, args.start, args.end, args.sample_rate, args.hop_length)
    midi_values = load_reference_pitches(args.midi, args.start, args.end, args.midi_shift_seconds)
    audio_stats = summarize(audio_values)
    midi_stats = summarize(midi_values)
    raw_delta = None
    if audio_stats is not None and midi_stats is not None:
        raw_delta = float(audio_stats["median"] - midi_stats["median"])
    octave_delta = octave_normalized_delta(audio_stats, midi_stats)
    result: dict[str, Any] = {
        "audio": str(args.audio),
        "midi": str(args.midi),
        "window": {"start": args.start, "end": args.end},
        "midi_shift_seconds": args.midi_shift_seconds,
        "audio_pitch_stats": audio_stats,
        "midi_pitch_stats": midi_stats,
        "raw_median_delta_semitones": raw_delta,
        "octave_normalized_delta_semitones": octave_delta,
        "diagnosis": likely_register_issue(raw_delta, octave_delta),
    }
    if args.output_json is not None:
        args.output_json.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
