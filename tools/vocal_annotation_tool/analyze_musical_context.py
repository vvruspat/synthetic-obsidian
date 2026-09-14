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
MAJOR_KEY_PROFILE = np.asarray((6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88))
MINOR_KEY_PROFILE = np.asarray((6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17))


@dataclass(frozen=True)
class ChordTemplate:
    suffix: str
    intervals: tuple[int, ...]


CHORD_TEMPLATES = (
    ChordTemplate("(5)", (0, 7)),
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


def phase_aligned_beat_times(duration: float, bpm: float, detected: np.ndarray) -> np.ndarray:
    """Build a stable tempo grid while retaining the beat tracker's phase."""
    beat_step = 60.0 / max(bpm, 1.0)
    usable = np.asarray(detected, dtype=np.float64)
    usable = usable[np.isfinite(usable) & (usable >= 0.0) & (usable <= duration)]
    if usable.size == 0:
        return regular_beat_times(duration, bpm)

    angles = 2.0 * np.pi * np.mod(usable, beat_step) / beat_step
    phase_angle = float(np.arctan2(np.mean(np.sin(angles)), np.mean(np.cos(angles))))
    phase = (phase_angle % (2.0 * np.pi)) * beat_step / (2.0 * np.pi)
    first_index = int(np.floor(-phase / beat_step))
    last_index = int(np.ceil((duration - phase) / beat_step))
    beats = phase + np.arange(first_index, last_index + 1, dtype=np.float64) * beat_step
    return beats[(beats >= 0.0) & (beats <= duration)]


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


def normalize_chroma(vector: np.ndarray) -> np.ndarray:
    values = np.maximum(0.0, np.asarray(vector, dtype=np.float64))
    total = float(np.sum(values))
    return values / total if total > 1e-8 else np.zeros(12, dtype=np.float64)


def chord_states() -> tuple[tuple[int, ChordTemplate], ...]:
    return tuple((root, template) for root in range(12) for template in CHORD_TEMPLATES)


def key_pitch_classes(key_name: str | None) -> set[int]:
    if not key_name:
        return set()
    parts = key_name.split()
    if not parts or parts[0] not in NOTE_NAMES:
        return set()
    tonic = NOTE_NAMES.index(parts[0])
    intervals = (0, 2, 3, 5, 7, 8, 10) if "minor" in key_name.lower() else (0, 2, 4, 5, 7, 9, 11)
    return {(tonic + interval) % 12 for interval in intervals}


def estimate_key(chroma: np.ndarray) -> tuple[str, float]:
    vector = normalize_chroma(np.median(chroma, axis=1) * 0.45 + np.mean(chroma, axis=1) * 0.55)
    if float(np.sum(vector)) <= 1e-8:
        return "C major", 0.0
    candidates: list[tuple[str, float]] = []
    for tonic in range(12):
        for mode, profile in (("major", MAJOR_KEY_PROFILE), ("minor", MINOR_KEY_PROFILE)):
            rotated = np.roll(profile, tonic)
            score = float(np.corrcoef(vector, rotated)[0, 1])
            candidates.append((f"{NOTE_NAMES[tonic]} {mode}", score))
    candidates.sort(key=lambda item: item[1], reverse=True)
    best_name, best_score = candidates[0]
    second_score = candidates[1][1]
    confidence = float(np.clip(0.35 + (best_score - second_score) * 1.8, 0.2, 0.95))
    return best_name, confidence


def score_chord_candidates(
    chroma_vector: np.ndarray,
    bass_vector: np.ndarray | None = None,
    key_name: str | None = None,
) -> list[tuple[str, float]]:
    chroma = normalize_chroma(chroma_vector)
    bass = normalize_chroma(bass_vector if bass_vector is not None else chroma_vector)
    if float(np.sum(chroma)) <= 1e-8:
        return [("N", 0.0)]

    scale = key_pitch_classes(key_name)
    quality_prior = {
        "": 0.055,
        "m": 0.055,
        "(5)": 0.025,
        "dim": -0.055,
        "aug": -0.065,
        "sus2": -0.025,
        "sus4": -0.025,
        "7": -0.035,
        "maj7": -0.055,
        "m7": -0.055,
    }
    scored: list[tuple[str, float]] = []
    for root, template in chord_states():
        pitch_classes = {(root + interval) % 12 for interval in template.intervals}
        weights = np.zeros(12, dtype=np.float64)
        for interval in template.intervals:
            pitch_class = (root + interval) % 12
            weights[pitch_class] = 1.0 if interval == 0 else 0.88 if interval in (3, 4) else 0.76
        template_norm = float(np.linalg.norm(weights))
        chroma_norm = float(np.linalg.norm(chroma))
        cosine = float(np.dot(chroma, weights) / max(1e-8, chroma_norm * template_norm))
        covered = float(sum(chroma[pitch_class] for pitch_class in pitch_classes))
        outside = max(0.0, 1.0 - covered)
        root_evidence = float(chroma[root])
        bass_evidence = float(bass[root])
        complexity_penalty = 0.035 * max(0, len(template.intervals) - 3)
        power_penalty = 0.035 if template.suffix == "(5)" else 0.0
        scale_fit = 0.0
        if scale:
            outside_scale = len(pitch_classes - scale)
            scale_fit = 0.045 if outside_scale == 0 else -0.035 * outside_scale
        score = (
            cosine * 0.56
            + covered * 0.32
            + root_evidence * 0.08
            + bass_evidence * 0.16
            - outside * 0.28
            - complexity_penalty
            - power_penalty
            + quality_prior.get(template.suffix, 0.0)
            + scale_fit
        )
        scored.append((chord_name(root, template), score))
    return sorted(scored, key=lambda item: item[1], reverse=True)


def score_chord(
    chroma_vector: np.ndarray,
    bass_vector: np.ndarray | None = None,
    key_name: str | None = None,
) -> tuple[str, float]:
    candidates = score_chord_candidates(chroma_vector, bass_vector, key_name)
    if not candidates or candidates[0][0] == "N":
        return "N", 0.0
    best_name, best_score = candidates[0]
    second_score = candidates[1][1] if len(candidates) > 1 else best_score - 0.1
    confidence = float(np.clip(0.35 + (best_score - second_score) * 3.0, 0.2, 0.95))
    return best_name, confidence


def subbeat_boundaries(beat_times: np.ndarray, duration: float) -> np.ndarray:
    beats = sorted(float(value) for value in beat_times if 0.0 < value < duration)
    boundaries = [0.0, *beats, duration]
    for left, right in zip(beats, beats[1:]):
        if right - left >= 0.24:
            boundaries.append((left + right) * 0.5)
    return np.asarray(sorted(set(round(value, 6) for value in boundaries)), dtype=np.float64)


def aggregate_feature(feature: np.ndarray, mask: np.ndarray) -> np.ndarray:
    if not np.any(mask):
        return np.zeros(12, dtype=np.float64)
    values = feature[:, mask]
    return np.median(values, axis=1) * 0.65 + np.mean(values, axis=1) * 0.35


def decode_chord_sequence(
    chroma_cells: list[np.ndarray],
    bass_cells: list[np.ndarray],
    key_name: str | None = None,
) -> tuple[list[int], list[list[tuple[str, float]]]]:
    states = chord_states()
    candidates_by_cell = [
        score_chord_candidates(chroma, bass, key_name)
        for chroma, bass in zip(chroma_cells, bass_cells)
    ]
    if not candidates_by_cell:
        return [], []

    score_maps = [{name: score for name, score in candidates} for candidates in candidates_by_cell]
    state_names = [chord_name(root, template) for root, template in states]
    previous = np.asarray([score_maps[0][name] for name in state_names], dtype=np.float64)
    backpointers: list[np.ndarray] = []
    stay_bonus = 0.10
    change_penalty = 0.055

    for cell_scores in score_maps[1:]:
        order = np.argsort(previous)
        best_index = int(order[-1])
        second_index = int(order[-2]) if len(order) > 1 else best_index
        current = np.empty_like(previous)
        pointers = np.empty(len(states), dtype=np.int32)
        for state_index, name in enumerate(state_names):
            same_score = previous[state_index] + stay_bonus
            other_index = second_index if best_index == state_index else best_index
            changed_score = previous[other_index] - change_penalty
            if same_score >= changed_score:
                current[state_index] = same_score + cell_scores[name]
                pointers[state_index] = state_index
            else:
                current[state_index] = changed_score + cell_scores[name]
                pointers[state_index] = other_index
        current -= float(np.max(current))
        previous = current
        backpointers.append(pointers)

    state_index = int(np.argmax(previous))
    decoded = [state_index]
    for pointers in reversed(backpointers):
        state_index = int(pointers[state_index])
        decoded.append(state_index)
    decoded.reverse()
    return decoded, candidates_by_cell


def suppress_weak_micro_changes(
    decoded: list[int],
    candidates_by_cell: list[list[tuple[str, float]]],
) -> list[int]:
    """Remove short state changes unless their local evidence is decisive."""
    output = list(decoded)
    state_names = [chord_name(root, template) for root, template in chord_states()]
    for _ in range(2):
        runs: list[tuple[int, int, int]] = []
        start = 0
        for index in range(1, len(output) + 1):
            if index == len(output) or output[index] != output[start]:
                runs.append((start, index, output[start]))
                start = index
        changed = False
        for run_index, (start, end, state_index) in enumerate(runs):
            if end - start > 2 or run_index == 0 or run_index == len(runs) - 1:
                continue
            neighbor_states = {runs[run_index - 1][2], runs[run_index + 1][2]}
            current_name = state_names[state_index]
            current_score = float(np.mean([
                dict(candidates_by_cell[cell]).get(current_name, 0.0)
                for cell in range(start, end)
            ]))
            neighbor_scores = []
            for neighbor_state in neighbor_states:
                neighbor_name = state_names[neighbor_state]
                score = float(np.mean([
                    dict(candidates_by_cell[cell]).get(neighbor_name, 0.0)
                    for cell in range(start, end)
                ]))
                neighbor_scores.append((score, neighbor_state))
            best_neighbor_score, best_neighbor_state = max(neighbor_scores)
            if current_score - best_neighbor_score < 0.16:
                output[start:end] = [best_neighbor_state] * (end - start)
                changed = True
        if not changed:
            break
    return output


def conservative_chord_name(
    name: str,
    selected_score: float,
    ranked: list[tuple[str, float]],
) -> tuple[str, float]:
    """Prefer a stable triad over extensions supported only by mix residue."""
    root = chord_root_name(name)
    suffix = name[len(root):]
    if suffix in {"", "m", "(5)"}:
        return name, selected_score
    simple = [
        (candidate_name, score)
        for candidate_name, score in ranked
        if chord_root_name(candidate_name) == root
        and candidate_name[len(root):] in {"", "m", "(5)"}
    ]
    if not simple:
        return name, selected_score
    simple_name, simple_score = max(simple, key=lambda item: item[1])
    if suffix == "7":
        threshold = 0.08
    elif suffix in {"sus2", "sus4"}:
        threshold = 0.20
    elif suffix in {"maj7", "m7"}:
        threshold = 0.16
    else:
        threshold = 0.13
    if selected_score - simple_score < threshold:
        return simple_name, simple_score
    return name, selected_score


def suppress_tiny_segments(
    segments: list[dict[str, object]],
    minimum_duration: float,
) -> list[dict[str, object]]:
    output = [dict(segment) for segment in segments]
    index = 0
    while index < len(output):
        segment = output[index]
        if float(segment["end"]) - float(segment["start"]) >= minimum_duration or len(output) == 1:
            index += 1
            continue
        previous = output[index - 1] if index > 0 else None
        following = output[index + 1] if index + 1 < len(output) else None
        root = chord_root_name(str(segment["name"]))
        if previous is not None and chord_root_name(str(previous["name"])) == root:
            previous["end"] = segment["end"]
        elif following is not None and chord_root_name(str(following["name"])) == root:
            following["start"] = segment["start"]
        elif previous is not None and following is not None and previous["name"] == following["name"]:
            previous["end"] = following["end"]
            output.pop(index + 1)
        elif previous is not None and (
            following is None
            or float(previous.get("confidence", 0.0)) >= float(following.get("confidence", 0.0))
        ):
            previous["end"] = segment["end"]
        elif following is not None:
            following["start"] = segment["start"]
        output.pop(index)
        index = max(0, index - 1)
    return output


def harmonic_novelty(chroma: np.ndarray, bass: np.ndarray, window_frames: int) -> np.ndarray:
    combined = normalize_feature_columns(chroma) * 0.78 + normalize_feature_columns(bass) * 0.22
    cumulative = np.pad(np.cumsum(combined, axis=1), ((0, 0), (1, 0)))
    novelty = np.zeros(combined.shape[1], dtype=np.float64)
    for index in range(window_frames, combined.shape[1] - window_frames):
        left = cumulative[:, index] - cumulative[:, index - window_frames]
        right = cumulative[:, index + window_frames] - cumulative[:, index]
        denominator = float(np.linalg.norm(left) * np.linalg.norm(right))
        novelty[index] = 1.0 - float(np.dot(left, right) / max(1e-8, denominator))
    return novelty


def normalize_feature_columns(feature: np.ndarray) -> np.ndarray:
    denominator = np.sum(np.maximum(feature, 0.0), axis=0, keepdims=True)
    return np.maximum(feature, 0.0) / np.maximum(denominator, 1e-8)


def refine_boundary(
    boundary: float,
    frame_times: np.ndarray,
    novelty: np.ndarray,
    search_radius: float,
    lower: float,
    upper: float,
) -> float:
    mask = (
        (frame_times >= max(lower, boundary - search_radius))
        & (frame_times <= min(upper, boundary + search_radius))
    )
    indices = np.flatnonzero(mask)
    if indices.size == 0:
        return boundary
    local = novelty[indices]
    distances = np.abs(frame_times[indices] - boundary) / max(search_radius, 1e-6)
    scores = local - distances * 0.035
    chosen = int(indices[int(np.argmax(scores))])
    return float(frame_times[chosen])


def estimate_chords_from_features(
    chroma: np.ndarray,
    bass: np.ndarray,
    frame_times: np.ndarray,
    beat_times: np.ndarray,
    duration: float,
    hop_seconds: float,
    key_name: str | None = None,
) -> list[dict[str, object]]:
    boundaries = subbeat_boundaries(beat_times, duration)
    chroma_cells: list[np.ndarray] = []
    bass_cells: list[np.ndarray] = []
    valid_boundaries = [float(boundaries[0])]
    for start, end in zip(boundaries, boundaries[1:]):
        mask = (frame_times >= start) & (frame_times < end)
        if end - start < 0.08 or not np.any(mask):
            continue
        chroma_cells.append(aggregate_feature(chroma, mask))
        bass_cells.append(aggregate_feature(bass, mask))
        valid_boundaries.append(float(end))
    if not chroma_cells:
        return []

    decoded, candidates_by_cell = decode_chord_sequence(chroma_cells, bass_cells, key_name)
    decoded = suppress_weak_micro_changes(decoded, candidates_by_cell)
    states = chord_states()
    state_names = [chord_name(root, template) for root, template in states]
    runs: list[tuple[int, int, int]] = []
    run_start = 0
    for index in range(1, len(decoded) + 1):
        if index == len(decoded) or decoded[index] != decoded[run_start]:
            runs.append((run_start, index, decoded[run_start]))
            run_start = index

    window_frames = max(2, int(round(0.18 / max(hop_seconds, 1e-4))))
    novelty = harmonic_novelty(chroma, bass, window_frames)
    refined = [valid_boundaries[0]]
    median_beat = float(np.median(np.diff(beat_times))) if beat_times.size >= 2 else 0.5
    search_radius = min(0.32, max(0.08, median_beat * 0.38))
    for run_index in range(1, len(runs)):
        coarse = valid_boundaries[runs[run_index][0]]
        lower = refined[-1] + 0.08
        next_coarse = valid_boundaries[runs[run_index][1]] if run_index + 1 < len(runs) else duration
        upper = max(lower, next_coarse - 0.08)
        refined.append(refine_boundary(coarse, frame_times, novelty, search_radius, lower, upper))
    refined.append(duration)

    segments: list[dict[str, object]] = []
    for run_index, (start_cell, end_cell, state_index) in enumerate(runs):
        start = max(0.0, float(refined[run_index]))
        end = min(duration, float(refined[run_index + 1]))
        if end - start < 0.08:
            continue
        name = state_names[state_index]
        scores: dict[str, list[float]] = {}
        for candidates in candidates_by_cell[start_cell:end_cell]:
            for candidate_name, score in candidates:
                scores.setdefault(candidate_name, []).append(score)
        ranked = sorted(
            ((candidate_name, float(np.mean(values))) for candidate_name, values in scores.items()),
            key=lambda item: item[1],
            reverse=True,
        )
        selected_score = float(np.mean([
            dict(candidates).get(name, 0.0)
            for candidates in candidates_by_cell[start_cell:end_cell]
        ]))
        name, selected_score = conservative_chord_name(name, selected_score, ranked)
        competing = max((score for candidate_name, score in ranked if candidate_name != name), default=selected_score - 0.1)
        confidence = float(np.clip(0.38 + (selected_score - competing) * 2.5, 0.2, 0.95))
        alternatives = [candidate_name for candidate_name, _ in ranked if candidate_name != name][:3]
        segments.append(
            {
                "start": round(start, 4),
                "end": round(end, 4),
                "name": name,
                "confidence": round(confidence, 3),
                "alternatives": alternatives,
            }
        )
    minimum_duration = max(0.12, median_beat * 0.30)
    return merge_segments(suppress_tiny_segments(segments, minimum_duration), "name")


def estimate_chords(
    y: np.ndarray,
    sr: int,
    hop_length: int,
    beat_times: np.ndarray,
    duration: float,
) -> tuple[list[dict[str, object]], str, float]:
    harmonic = librosa.effects.harmonic(y, margin=4.0)
    tuning = scalar(librosa.estimate_tuning(y=harmonic, sr=sr), 0.0)
    chroma_cqt = librosa.feature.chroma_cqt(
        y=harmonic,
        sr=sr,
        hop_length=hop_length,
        tuning=tuning,
    )
    chroma_cens = librosa.feature.chroma_cens(
        y=harmonic,
        sr=sr,
        hop_length=hop_length,
        tuning=tuning,
    )
    frame_count = min(chroma_cqt.shape[1], chroma_cens.shape[1])
    chroma = chroma_cqt[:, :frame_count] * 0.72 + chroma_cens[:, :frame_count] * 0.28
    key_name, key_confidence = estimate_key(chroma)
    bass = librosa.feature.chroma_cqt(
        y=harmonic,
        sr=sr,
        hop_length=hop_length,
        tuning=tuning,
        fmin=librosa.note_to_hz("C1"),
        n_octaves=3,
    )[:, :frame_count]
    frame_times = librosa.frames_to_time(np.arange(frame_count), sr=sr, hop_length=hop_length)
    return (
        estimate_chords_from_features(
            chroma,
            bass,
            frame_times,
            beat_times,
            duration,
            hop_length / sr,
            key_name,
        ),
        key_name,
        key_confidence,
    )


def analyze(path: Path) -> dict[str, object]:
    y, sr = librosa.load(path, sr=22050, mono=True)
    if y.size == 0:
        raise RuntimeError("audio file is empty")
    duration = float(librosa.get_duration(y=y, sr=sr))
    hop_length = 512
    _, detected_beat_times, onset_envelope = estimate_beats(y, sr, hop_length)
    global_bpm = rounded_bpm(estimate_consensus_tempo(y, sr))
    beat_times = phase_aligned_beat_times(duration, global_bpm, detected_beat_times)
    tempos = tempo_segments(beat_times, duration, global_bpm, force_constant=True)
    signatures = infer_time_signature(detected_beat_times, onset_envelope, sr, hop_length, duration)
    chords, key_name, key_confidence = estimate_chords(y, sr, hop_length, beat_times, duration)
    return {
        "duration": round(duration, 4),
        "bpm": global_bpm,
        "tempo_segments": tempos,
        "time_signatures": signatures,
        "key": key_name,
        "key_confidence": round(key_confidence, 3),
        "chords": chords,
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("audio", type=Path)
    args = parser.parse_args()
    print(json.dumps(analyze(args.audio.expanduser().resolve()), ensure_ascii=False))


if __name__ == "__main__":
    main()
