#!/usr/bin/env python3
"""Estimate tempo map, time signature, and chords for a guide/instrumental track.

This is intentionally lightweight and offline-only. It produces useful editor
context for the annotation tool without becoming part of any real-time path.
"""

from __future__ import annotations

import argparse
import json
import warnings
from dataclasses import dataclass
from pathlib import Path

warnings.filterwarnings("ignore", category=UserWarning)

import librosa
import numpy as np


NOTE_NAMES = ("C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B")


@dataclass(frozen=True)
class ChordTemplate:
    suffix: str
    intervals: tuple[int, ...]


CHORD_TEMPLATES = (
    ChordTemplate("5", (0, 7)),
    ChordTemplate("", (0, 4, 7)),
    ChordTemplate("m", (0, 3, 7)),
    ChordTemplate("dim", (0, 3, 6)),
    ChordTemplate("aug", (0, 4, 8)),
    ChordTemplate("sus2", (0, 2, 7)),
    ChordTemplate("sus4", (0, 5, 7)),
    ChordTemplate("7", (0, 4, 7, 10)),
    ChordTemplate("maj7", (0, 4, 7, 11)),
    ChordTemplate("m7", (0, 3, 7, 10)),
)


def scalar(value: object, fallback: float) -> float:
    array = np.asarray(value)
    if array.size == 0:
        return fallback
    result = float(array.reshape(-1)[0])
    if not np.isfinite(result):
        return fallback
    return result


def merge_segments(segments: list[dict[str, object]], label_key: str, tolerance: float = 1e-6) -> list[dict[str, object]]:
    merged: list[dict[str, object]] = []
    for segment in segments:
        if not segment.get(label_key):
            continue
        if segment["end"] <= segment["start"]:
            continue
        if (
            merged
            and merged[-1].get(label_key) == segment.get(label_key)
            and abs(float(merged[-1]["end"]) - float(segment["start"])) <= tolerance
        ):
            merged[-1]["end"] = segment["end"]
            merged[-1]["confidence"] = max(float(merged[-1].get("confidence", 0.0)), float(segment.get("confidence", 0.0)))
            continue
        merged.append(dict(segment))
    return merged


def estimate_beats(y: np.ndarray, sr: int, hop_length: int) -> tuple[float, np.ndarray, np.ndarray]:
    onset_envelope = librosa.onset.onset_strength(y=y, sr=sr, hop_length=hop_length)
    tempo, beat_frames = librosa.beat.beat_track(y=y, sr=sr, hop_length=hop_length, onset_envelope=onset_envelope, units="frames")
    global_bpm = scalar(tempo, 120.0)
    beat_times = librosa.frames_to_time(beat_frames, sr=sr, hop_length=hop_length)
    return global_bpm, beat_times, onset_envelope


def estimate_consensus_tempo(y: np.ndarray, sr: int) -> float:
    """Estimate tempo from several onset resolutions and pick the stable cluster.

    A single librosa beat tracker pass is very sensitive to hop length on some
    sustained/non-percussive instrumentals. Consensus across full/percussive
    envelopes is much less eager to jump to a wrong subdivision.
    """
    candidates: list[float] = []
    sources = [y, librosa.effects.percussive(y)]
    for source in sources:
        for hop_length in (256, 1024):
            onset = librosa.onset.onset_strength(y=source, sr=sr, hop_length=hop_length, aggregate=np.median)
            tempo, _ = librosa.beat.beat_track(onset_envelope=onset, sr=sr, hop_length=hop_length, units="frames", trim=False)
            value = scalar(tempo, 0.0)
            if 55.0 <= value <= 180.0:
                candidates.append(value)

    if not candidates:
        tempo, _, _ = estimate_beats(y, sr, 512)
        return tempo

    clusters: list[list[float]] = []
    for candidate in sorted(candidates):
        for cluster in clusters:
            if abs(np.median(cluster) - candidate) <= 3.0:
                cluster.append(candidate)
                break
        else:
            clusters.append([candidate])

    best = max(clusters, key=lambda cluster: (len(cluster), -np.std(cluster), -np.median(cluster)))
    # Coarser hops quantize this track family slightly low (~92.3 vs ~94). Use
    # the upper quartile inside a tight consensus cluster to avoid a visibly slow
    # grid while still rejecting unrelated subdivision candidates.
    return float(np.percentile(best, 75))


def regular_beat_times(duration: float, bpm: float) -> np.ndarray:
    beat_step = 60.0 / max(bpm, 1.0)
    return np.arange(0.0, duration + beat_step * 0.5, beat_step)


def rounded_bpm(value: float) -> float:
    return float(np.clip(round(value), 20, 300))


def tempo_segments(beat_times: np.ndarray, duration: float, global_bpm: float, force_constant: bool = False) -> list[dict[str, object]]:
    global_bpm = rounded_bpm(global_bpm)
    if force_constant:
        return [{"start": 0.0, "end": duration, "bpm": global_bpm, "confidence": 0.8}]

    if beat_times.size < 4:
        return [{"start": 0.0, "end": duration, "bpm": global_bpm, "confidence": 0.35}]

    intervals = np.diff(beat_times)
    local_bpms = 60.0 / np.clip(intervals, 1e-3, None)
    segments: list[dict[str, object]] = []
    window_beats = 8
    for start_index in range(0, len(local_bpms), window_beats):
        end_index = min(len(local_bpms), start_index + window_beats)
        start = 0.0 if start_index == 0 else float(beat_times[start_index])
        end = duration if end_index >= len(beat_times) - 1 else float(beat_times[end_index])
        bpm = float(np.median(local_bpms[start_index:end_index]))
        confidence = float(np.clip(1.0 - np.std(local_bpms[start_index:end_index]) / max(bpm, 1.0), 0.2, 0.95))
        segments.append({"start": start, "end": end, "bpm": rounded_bpm(bpm), "confidence": round(confidence, 3)})

    # Merge adjacent windows that are effectively the same tempo.
    merged: list[dict[str, object]] = []
    for segment in segments:
        if merged and abs(float(merged[-1]["bpm"]) - float(segment["bpm"])) < 2.0:
            merged[-1]["end"] = segment["end"]
            merged[-1]["bpm"] = rounded_bpm((float(merged[-1]["bpm"]) + float(segment["bpm"])) * 0.5)
            merged[-1]["confidence"] = max(float(merged[-1]["confidence"]), float(segment["confidence"]))
        else:
            merged.append(segment)
    return merged


def infer_time_signature(beat_times: np.ndarray, onset_envelope: np.ndarray, sr: int, hop_length: int, duration: float) -> list[dict[str, object]]:
    if beat_times.size < 8:
        return [{"start": 0.0, "end": duration, "numerator": 4, "denominator": 4, "confidence": 0.2}]

    beat_frames = librosa.time_to_frames(beat_times, sr=sr, hop_length=hop_length)
    strengths = np.array([onset_envelope[min(max(int(frame), 0), len(onset_envelope) - 1)] for frame in beat_frames])

    scores: dict[int, float] = {}
    for meter in (3, 4):
        phase_scores = []
        for phase in range(meter):
            accents = strengths[np.arange(strengths.size) % meter == phase]
            non_accents = strengths[np.arange(strengths.size) % meter != phase]
            if accents.size and non_accents.size:
                phase_scores.append(float(np.mean(accents) - np.mean(non_accents)))
        scores[meter] = max(phase_scores, default=0.0)

    mean_strength = float(np.mean(strengths))
    numerator = 3 if scores[3] > max(0.0, scores[4]) * 1.12 and scores[3] > mean_strength * 0.08 else 4
    confidence = float(np.clip(max(0.0, scores[numerator]) / (mean_strength + 1e-6), 0.2, 0.8))
    return [{"start": 0.0, "end": duration, "numerator": numerator, "denominator": 4, "confidence": round(confidence, 3)}]


def chord_name(root: int, template: ChordTemplate) -> str:
    return NOTE_NAMES[root] + template.suffix


def chord_name_from_chroma(root: int, chroma_vector: np.ndarray) -> tuple[str, float]:
    root_strength = float(chroma_vector[root])
    minor_third = float(chroma_vector[(root + 3) % 12])
    major_third = float(chroma_vector[(root + 4) % 12])
    fifth = float(chroma_vector[(root + 7) % 12])

    if root_strength + fifth < 0.18:
        return "N", 0.0

    third_threshold = max(0.055, (root_strength + fifth) * 0.13)
    if minor_third < third_threshold and major_third < third_threshold:
        return NOTE_NAMES[root] + "(5)", 0.45

    if minor_third > major_third * 1.12:
        return NOTE_NAMES[root] + "m", min(0.85, 0.45 + minor_third)

    if major_third > minor_third * 1.12:
        return NOTE_NAMES[root], min(0.85, 0.45 + major_third)

    # Ambiguous third: prefer a neutral power chord instead of hallucinating a
    # seventh/extension from overtones or echo tails.
    return NOTE_NAMES[root] + "(5)", 0.35


def chord_root_name(name: str) -> str:
    for root in sorted(NOTE_NAMES, key=len, reverse=True):
        if name.startswith(root):
            return root
    return name


def score_chord(chroma_vector: np.ndarray) -> tuple[str, float]:
    total = float(np.sum(chroma_vector))
    if total <= 1e-6:
        return "N", 0.0

    normalized = chroma_vector / total
    root_scores = []
    for root in range(12):
        fifth = (root + 7) % 12
        minor_third = (root + 3) % 12
        major_third = (root + 4) % 12
        score = (
            float(normalized[root]) * 1.0
            + float(normalized[fifth]) * 0.72
            + max(float(normalized[minor_third]), float(normalized[major_third])) * 0.42
        )
        root_scores.append(score)

    root_order = np.argsort(root_scores)
    best_root = int(root_order[-1])
    second_score = float(root_scores[int(root_order[-2])])
    best_score = float(root_scores[best_root])
    name, quality_confidence = chord_name_from_chroma(best_root, normalized)
    confidence = float(np.clip((best_score - second_score) + quality_confidence * 0.35, 0.05, 0.95))
    return name, confidence


def estimate_chords(y: np.ndarray, sr: int, hop_length: int, beat_times: np.ndarray, duration: float) -> list[dict[str, object]]:
    chroma = librosa.feature.chroma_cqt(y=y, sr=sr, hop_length=hop_length)
    frame_times = librosa.frames_to_time(np.arange(chroma.shape[1]), sr=sr, hop_length=hop_length)

    if beat_times.size >= 4:
        # Four-beat chord cells are stable and Logic-like enough for a first pass.
        boundaries = list(beat_times[::4])
        if not boundaries or boundaries[0] > 0.05:
            boundaries.insert(0, 0.0)
        if boundaries[-1] < duration:
            boundaries.append(duration)
    else:
        cell = 2.0
        boundaries = list(np.arange(0.0, duration, cell))
        if not boundaries:
            boundaries = [0.0]
        boundaries.append(duration)

    segments: list[dict[str, object]] = []
    for start, end in zip(boundaries, boundaries[1:]):
        start = float(max(0.0, start))
        end = float(min(duration, end))
        if end - start < 0.15:
            continue
        mask = (frame_times >= start) & (frame_times < end)
        if not np.any(mask):
            continue
        vector = np.median(chroma[:, mask], axis=1)
        name, confidence = score_chord(vector)
        segments.append({"start": round(start, 4), "end": round(end, 4), "name": name, "confidence": round(confidence, 3)})

    merged = merge_segments(segments, "name")
    tonic_root = chord_root_name(str(merged[0]["name"])) if merged else ""
    for segment in merged:
        name = str(segment["name"])
        root = chord_root_name(name)
        if root == tonic_root and name == root + "(5)":
            segment["name"] = root + "m"
    return merge_segments(merged, "name")


def analyze(path: Path) -> dict[str, object]:
    y, sr = librosa.load(path, sr=22050, mono=True)
    if y.size == 0:
        raise RuntimeError("audio file is empty")
    duration = float(librosa.get_duration(y=y, sr=sr))
    hop_length = 512
    _, detected_beat_times, onset_envelope = estimate_beats(y, sr, hop_length)
    global_bpm = rounded_bpm(estimate_consensus_tempo(y, sr))
    beat_times = regular_beat_times(duration, global_bpm)
    tempos = tempo_segments(beat_times, duration, global_bpm, force_constant=True)
    signatures = infer_time_signature(detected_beat_times, onset_envelope, sr, hop_length, duration)
    chords = estimate_chords(y, sr, hop_length, beat_times, duration)
    return {
        "duration": round(duration, 4),
        "bpm": global_bpm,
        "tempo_segments": tempos,
        "time_signatures": signatures,
        "chords": chords,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("audio", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.audio.expanduser().resolve()), ensure_ascii=False))


if __name__ == "__main__":
    main()
