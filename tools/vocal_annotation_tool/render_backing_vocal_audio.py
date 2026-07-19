#!/usr/bin/env python3
"""Render backing-vocal audio from a lead vocal and generated backing notes.

This is an offline-only prototype for the Vocal Annotation Tool. It uses WORLD
to keep the lead vocal's spectral envelope / aperiodicity while replacing the
voiced F0 contour with the generated backing-vocal MIDI notes.
"""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import numpy as np
import pyworld as pw
import soundfile as sf


WORLD_FRAME_PERIOD_MS = 5.0


def midi_to_hz(midi: float) -> float:
    return 440.0 * (2.0 ** ((midi - 69.0) / 12.0))


def load_mono(path: Path) -> tuple[np.ndarray, int]:
    audio, sample_rate = sf.read(str(path), dtype="float64", always_2d=True)
    mono = np.mean(audio, axis=1)
    return mono, int(sample_rate)


def note_pitch_at(note: dict[str, object], time_sec: float) -> float:
    curve = note.get("curve")
    if not isinstance(curve, list) or len(curve) == 0:
        return float(note.get("pitch_exact", note.get("pitch", 60.0)))

    points: list[tuple[float, float]] = []
    for value in curve:
        if not isinstance(value, dict):
            continue
        try:
            points.append((float(value.get("time", 0.0)), float(value.get("midi", 60.0))))
        except Exception:
            continue

    if not points:
        return float(note.get("pitch_exact", note.get("pitch", 60.0)))

    points.sort(key=lambda item: item[0])
    if time_sec <= points[0][0]:
        return points[0][1]
    if time_sec >= points[-1][0]:
        return points[-1][1]

    for (left_time, left_midi), (right_time, right_midi) in zip(points, points[1:]):
        if left_time <= time_sec <= right_time:
            span = max(1e-6, right_time - left_time)
            alpha = (time_sec - left_time) / span
            return left_midi + (right_midi - left_midi) * alpha

    return points[-1][1]


def build_target_f0(
    notes: list[dict[str, object]],
    source_f0: np.ndarray,
    frame_times: np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    target_f0 = np.zeros_like(source_f0)
    mask = np.zeros_like(source_f0)

    for note in notes:
        start = float(note.get("voiced_start", note.get("start", 0.0)))
        end = float(note.get("voiced_end", note.get("end", start)))
        if end <= start:
            continue

        active = (frame_times >= start) & (frame_times < end)
        if not np.any(active):
            continue

        indices = np.where(active)[0]
        for index in indices:
            midi = note_pitch_at(note, float(frame_times[index]))
            if math.isfinite(midi):
                # Keep truly unvoiced/consonant frames unvoiced. WORLD will
                # render their noise component from the source aperiodicity.
                target_f0[index] = midi_to_hz(midi) if source_f0[index] > 35.0 else 0.0
                mask[index] = 1.0

    # Soft time mask for audio gating after synthesis. This prevents WORLD from
    # producing room/noise tails across pauses while keeping note edges smooth.
    if len(mask) > 2:
        radius = max(1, int(round(0.015 / (WORLD_FRAME_PERIOD_MS / 1000.0))))
        kernel = np.hanning(radius * 2 + 1)
        kernel = kernel / np.sum(kernel)
        mask = np.convolve(mask, kernel, mode="same")
        mask = np.clip(mask, 0.0, 1.0)

    return target_f0, mask


def render(request_path: Path) -> dict[str, object]:
    request = json.loads(request_path.read_text(encoding="utf-8"))
    audio_path = Path(request["audio"])
    output_path = Path(request["output"])
    notes = request.get("backing_notes", [])
    if not isinstance(notes, list) or not notes:
        raise ValueError("request has no backing_notes")

    audio, sample_rate = load_mono(audio_path)
    if len(audio) == 0:
        raise ValueError(f"empty audio file: {audio_path}")

    f0, time_axis = pw.dio(audio, sample_rate, frame_period=WORLD_FRAME_PERIOD_MS)
    f0 = pw.stonemask(audio, f0, time_axis, sample_rate)
    spectral_envelope = pw.cheaptrick(audio, f0, time_axis, sample_rate)
    aperiodicity = pw.d4c(audio, f0, time_axis, sample_rate)

    target_f0, frame_mask = build_target_f0(notes, f0, time_axis)
    rendered = pw.synthesize(target_f0, spectral_envelope, aperiodicity, sample_rate, frame_period=WORLD_FRAME_PERIOD_MS)

    if len(rendered) < len(audio):
        rendered = np.pad(rendered, (0, len(audio) - len(rendered)))
    rendered = rendered[: len(audio)]

    sample_times = np.arange(len(rendered), dtype=np.float64) / float(sample_rate)
    sample_mask = np.interp(sample_times, time_axis, frame_mask, left=0.0, right=0.0)
    rendered = rendered * sample_mask

    peak = float(np.max(np.abs(rendered))) if len(rendered) else 0.0
    if peak > 0.98:
        rendered = rendered * (0.98 / peak)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    sf.write(str(output_path), rendered.astype(np.float32), sample_rate)

    return {
        "output": str(output_path),
        "sample_rate": sample_rate,
        "duration": len(rendered) / float(sample_rate),
        "notes": len(notes),
        "engine": "WORLD",
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("request_json", type=Path)
    args = parser.parse_args()
    print(json.dumps(render(args.request_json), ensure_ascii=False))


if __name__ == "__main__":
    main()
