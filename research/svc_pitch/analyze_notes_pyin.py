#!/usr/bin/env python3
"""Offline pYIN note detection for the Synthetic Obsidian piano roll.

The C++ YIN detector is intentionally lightweight, but vocals with breath,
vibrato and soft consonant transitions need a stronger offline pass. This script
uses librosa.pyin and returns compact note events as JSON on stdout.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path

import librosa
import numpy as np

_ASR_PIPELINE = None


def _fill_short_gaps(midi: np.ndarray, conf: np.ndarray, max_gap_frames: int) -> np.ndarray:
    result = midi.copy()
    n = len(result)
    i = 0
    while i < n:
        if result[i] >= 0:
            i += 1
            continue

        start = i
        while i < n and result[i] < 0:
            i += 1

        end = i - 1
        left = start - 1
        right = i
        gap_len = end - start + 1
        if left < 0 or right >= n or gap_len > max_gap_frames:
            continue
        if result[left] < 0 or result[right] < 0:
            continue
        if abs(result[left] - result[right]) > 2:
            continue

        for idx in range(start, end + 1):
            alpha = (idx - left) / (right - left)
            result[idx] = int(round(result[left] + (result[right] - result[left]) * alpha))
            conf[idx] = min(conf[left], conf[right]) * 0.65

    return result


def _median_smooth(midi: np.ndarray, radius: int = 4) -> np.ndarray:
    result = midi.copy()
    for idx, pitch in enumerate(midi):
        if pitch < 0:
            continue
        first = max(0, idx - radius)
        last = min(len(midi), idx + radius + 1)
        voiced = midi[first:last]
        voiced = voiced[voiced >= 0]
        if len(voiced) >= 3:
            result[idx] = int(np.median(voiced))
    return result


def _nearest_resample_track(
    source_values: np.ndarray,
    source_confidence: np.ndarray,
    source_times: np.ndarray,
    target_times: np.ndarray,
    max_distance_sec: float,
) -> tuple[np.ndarray, np.ndarray]:
    values = np.full(len(target_times), np.nan, dtype=np.float32)
    confidence = np.zeros(len(target_times), dtype=np.float32)
    if len(source_values) == 0 or len(source_times) == 0 or len(target_times) == 0:
        return values, confidence

    indices = np.searchsorted(source_times, target_times)
    for target_index, insertion in enumerate(indices):
        best_index = None
        best_distance = max_distance_sec
        for candidate in (insertion - 1, insertion):
            if candidate < 0 or candidate >= len(source_times):
                continue
            distance = abs(float(source_times[candidate] - target_times[target_index]))
            if distance <= best_distance:
                best_distance = distance
                best_index = int(candidate)

        if best_index is None:
            continue
        if not np.isfinite(source_values[best_index]):
            continue

        values[target_index] = float(source_values[best_index])
        confidence[target_index] = float(source_confidence[best_index])

    return values, confidence


def _compute_crepe_track(
    y: np.ndarray,
    sr: int,
    target_times: np.ndarray,
    hop_sec: float,
    sensitivity: float,
) -> tuple[np.ndarray, np.ndarray]:
    try:
        import torch
        import torchcrepe
    except Exception:
        return (
            np.full(len(target_times), np.nan, dtype=np.float32),
            np.zeros(len(target_times), dtype=np.float32),
        )

    try:
        crepe_sr = int(torchcrepe.SAMPLE_RATE)
        y_crepe = librosa.resample(y, orig_sr=sr, target_sr=crepe_sr)
        if y_crepe.size == 0:
            raise RuntimeError("empty resampled audio")

        hop_length = max(80, int(round(hop_sec * crepe_sr)))
        audio = torch.from_numpy(y_crepe.astype(np.float32, copy=False)).unsqueeze(0)
        model = os.environ.get("SO_CREPE_MODEL", "full")
        with torch.no_grad():
            pitch, periodicity = torchcrepe.predict(
                audio,
                crepe_sr,
                hop_length=hop_length,
                fmin=float(librosa.note_to_hz("C2")),
                fmax=float(librosa.note_to_hz("C6")),
                model=model,
                return_periodicity=True,
                batch_size=2048,
                device="cpu",
                pad=True,
            )

        pitch_np = pitch.squeeze(0).detach().cpu().numpy().astype(np.float32)
        periodicity_np = periodicity.squeeze(0).detach().cpu().numpy().astype(np.float32)
        if pitch_np.ndim > 1:
            pitch_np = pitch_np.reshape(-1)
        if periodicity_np.ndim > 1:
            periodicity_np = periodicity_np.reshape(-1)

        source_times = np.arange(len(pitch_np), dtype=np.float32) * hop_length / float(crepe_sr)
        valid = np.isfinite(pitch_np) & (pitch_np > 0.0)
        crepe_midi = np.full(len(pitch_np), np.nan, dtype=np.float32)
        crepe_midi[valid] = librosa.hz_to_midi(pitch_np[valid]).astype(np.float32)
        confidence = np.clip(np.nan_to_num(periodicity_np, nan=0.0), 0.0, 1.0).astype(np.float32)
        threshold = 0.22 - 0.08 * float(np.clip(sensitivity, 0.0, 1.0))
        crepe_midi[confidence < threshold] = np.nan
        return _nearest_resample_track(crepe_midi, confidence, source_times, target_times, hop_sec * 1.75)
    except Exception:
        return (
            np.full(len(target_times), np.nan, dtype=np.float32),
            np.zeros(len(target_times), dtype=np.float32),
        )


def _combine_pitch_tracks(
    pyin_midi: np.ndarray,
    pyin_conf: np.ndarray,
    crepe_midi: np.ndarray,
    crepe_conf: np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    combined = pyin_midi.copy()
    confidence = pyin_conf.copy()

    pyin_valid = np.isfinite(pyin_midi)
    crepe_valid = np.isfinite(crepe_midi)
    both = pyin_valid & crepe_valid
    agreement = np.abs(pyin_midi - crepe_midi)

    agree = both & (agreement <= 0.90)
    if np.any(agree):
        pyin_weight = np.clip(pyin_conf[agree], 0.05, 1.0) ** 1.2
        crepe_weight = np.clip(crepe_conf[agree], 0.05, 1.0) ** 1.6
        combined[agree] = (pyin_midi[agree] * pyin_weight + crepe_midi[agree] * crepe_weight) / (pyin_weight + crepe_weight)
        confidence[agree] = np.maximum(pyin_conf[agree], crepe_conf[agree])

    crepe_wins = crepe_valid & (
        ~pyin_valid
        | ((agreement > 0.90) & (crepe_conf >= np.maximum(0.42, pyin_conf + 0.14)))
        | ((pyin_conf < 0.22) & (crepe_conf >= 0.34))
    )
    combined[crepe_wins] = crepe_midi[crepe_wins]
    confidence[crepe_wins] = crepe_conf[crepe_wins]

    disagreement = both & (agreement > 0.90) & ~crepe_wins
    confidence[disagreement] *= 0.65
    return combined.astype(np.float32), np.clip(confidence, 0.0, 1.0).astype(np.float32)


def _midi_to_hz(midi_pitch: int) -> float:
    return 440.0 * (2.0 ** ((midi_pitch - 69) / 12.0))


def _detect_lyrics(path: Path) -> list[dict]:
    if os.environ.get("SO_ENABLE_LYRICS", "0") != "1":
        return []

    global _ASR_PIPELINE
    try:
        if _ASR_PIPELINE is None:
            from transformers import pipeline

            model_name = os.environ.get("SO_LYRICS_ASR_MODEL", "openai/whisper-tiny")
            _ASR_PIPELINE = pipeline(
                "automatic-speech-recognition",
                model=model_name,
                device=-1,
            )

        result = _ASR_PIPELINE(
            str(path),
            return_timestamps="word",
            generate_kwargs={"task": "transcribe"},
        )
    except Exception:
        return []

    chunks = result.get("chunks", []) if isinstance(result, dict) else []
    words: list[dict] = []
    for chunk in chunks:
        text = str(chunk.get("text", "")).strip()
        timestamp = chunk.get("timestamp")
        if not text or not timestamp or len(timestamp) != 2:
            continue
        start, end = timestamp
        if start is None:
            continue
        end = float(end) if end is not None else float(start) + 0.18
        words.append({"text": text[:32], "start": float(start), "end": end})

    return words


def _normalize_feature(values: np.ndarray) -> np.ndarray:
    values = np.nan_to_num(values, nan=0.0, posinf=0.0, neginf=0.0).astype(np.float32)
    peak = float(np.max(values)) if values.size else 0.0
    if peak <= 1.0e-8:
        return np.zeros_like(values, dtype=np.float32)
    return values / peak


def _detect_syllable_boundaries(
    y: np.ndarray,
    sr: int,
    hop_length: int,
    sensitivity: float,
) -> np.ndarray:
    onset_env = librosa.onset.onset_strength(
        y=y,
        sr=sr,
        hop_length=hop_length,
        aggregate=np.median,
    )
    rms = librosa.feature.rms(y=y, frame_length=2048, hop_length=hop_length)[0]
    rms_flux = np.maximum(0.0, np.diff(rms, prepend=rms[0]))

    n = min(len(onset_env), len(rms_flux))
    if n == 0:
        return np.asarray([], dtype=np.float32)

    strength = 0.68 * _normalize_feature(onset_env[:n]) + 0.32 * _normalize_feature(rms_flux[:n])
    if n >= 5:
        strength = np.convolve(strength, np.asarray([0.2, 0.6, 0.2], dtype=np.float32), mode="same")

    threshold = max(0.16, float(np.percentile(strength, 72.0 - 18.0 * sensitivity)))
    min_distance_frames = max(2, int(round(0.085 * sr / hop_length)))

    boundaries: list[float] = []
    last_peak = -min_distance_frames
    times = librosa.frames_to_time(np.arange(n), sr=sr, hop_length=hop_length)
    for idx in range(1, n - 1):
        if idx - last_peak < min_distance_frames:
            continue
        value = float(strength[idx])
        if value < threshold:
            continue
        if value < float(strength[idx - 1]) or value < float(strength[idx + 1]):
            continue
        boundaries.append(float(times[idx]))
        last_peak = idx

    rms_norm = _normalize_feature(rms[:n])
    flux_norm = _normalize_feature(rms_flux[:n])
    flux_threshold = max(0.12, float(np.percentile(flux_norm, 76.0 - 12.0 * sensitivity)))
    search_frames = max(3, int(round(0.22 * sr / hop_length)))

    for idx in range(2, n - 2):
        if float(flux_norm[idx]) >= flux_threshold and float(flux_norm[idx]) >= float(flux_norm[idx - 1]) and float(flux_norm[idx]) >= float(flux_norm[idx + 1]):
            if float(rms_norm[idx]) >= 0.08:
                boundaries.append(float(times[idx]))

        left = max(0, idx - search_frames)
        right = min(n, idx + search_frames + 1)
        if right - left < 5:
            continue

        left_peak = float(np.max(rms_norm[left:idx])) if idx > left else 0.0
        right_peak = float(np.max(rms_norm[idx + 1:right])) if idx + 1 < right else 0.0
        valley = float(rms_norm[idx])
        if right_peak < 0.18:
            continue
        if valley > min(left_peak, right_peak) * 0.52:
            continue
        if right_peak - valley < 0.18:
            continue

        rise_slice = rms_norm[idx:right]
        rise_candidates = np.nonzero(rise_slice >= valley + max(0.06, 0.25 * (right_peak - valley)))[0]
        rise_idx = idx + int(rise_candidates[0]) if rise_candidates.size else idx
        boundaries.append(float(times[min(rise_idx, n - 1)]))

    if not boundaries:
        return np.asarray([], dtype=np.float32)

    merged: list[float] = []
    min_distance_sec = min_distance_frames * hop_length / sr
    for boundary in sorted(boundaries):
        if merged and boundary - merged[-1] < min_distance_sec:
            continue
        merged.append(boundary)

    return np.asarray(merged, dtype=np.float32)


def _make_pitch_curve(
    midi_float: np.ndarray,
    conf: np.ndarray,
    times: np.ndarray,
    start_time: float,
    end_time: float,
) -> list[dict]:
    mask = (
        np.isfinite(midi_float)
        & (conf > 0.0)
        & (times >= start_time - 1.0e-6)
        & (times <= end_time + 1.0e-6)
    )
    if not np.any(mask):
        return []

    curve: list[tuple[float, float, float]] = []
    masked_times = times[mask]
    masked_midi = midi_float[mask]
    masked_conf = conf[mask]
    if len(masked_times) > 64:
        keep = np.linspace(0, len(masked_times) - 1, 64).round().astype(np.int32)
        masked_times = masked_times[keep]
        masked_midi = masked_midi[keep]
        masked_conf = masked_conf[keep]

    last_offset = -1.0
    for time_sec, midi_value, confidence in zip(masked_times, masked_midi, masked_conf):
        offset = float(time_sec - start_time)
        if curve and offset - last_offset < 0.018:
            continue
        curve.append((offset, float(midi_value), float(confidence)))
        last_offset = offset

    return ";".join(f"{offset:.3f}:{midi_value:.2f}:{confidence:.3f}" for offset, midi_value, confidence in curve)


def _decorate_note(
    pitch: int,
    start_time: float,
    end_time: float,
    confidence: float,
    midi_float: np.ndarray,
    f0: np.ndarray,
    conf: np.ndarray,
    times: np.ndarray,
    syllable_start: bool = False,
) -> dict:
    mask = np.isfinite(f0) & (f0 > 0.0) & (times >= start_time - 1.0e-6) & (times <= end_time + 1.0e-6)
    avg_hz = float(np.mean(f0[mask])) if np.any(mask) else 0.0
    midi_mask = np.isfinite(midi_float) & (times >= start_time - 1.0e-6) & (times <= end_time + 1.0e-6)
    pitch_exact = None
    if np.any(midi_mask):
        duration = max(0.0, end_time - start_time)
        trim = min(0.08, duration * 0.22)
        core_mask = midi_mask
        if duration >= 0.18 and trim > 0.0:
            core_mask = midi_mask & (times >= start_time + trim) & (times <= end_time - trim)
        pitch_exact = _representative_pitch(midi_float[core_mask], conf[core_mask]) if np.any(core_mask) else None
        if pitch_exact is None:
            pitch_exact = _representative_pitch(midi_float[midi_mask], conf[midi_mask])
    if pitch_exact is None:
        pitch_exact = float(pitch)

    pitch = int(np.clip(np.rint(pitch_exact), 0, 127))
    cents = 0.0
    if avg_hz > 0.0:
        cents = float(np.clip(1200.0 * np.log2(avg_hz / _midi_to_hz(pitch)), -50.0, 50.0))

    return {
        "pitch": int(pitch),
        "pitch_exact": float(pitch_exact),
        "start": float(start_time),
        "duration": float(end_time - start_time),
        "confidence": float(np.clip(confidence, 0.0, 1.0)),
        "cents": cents,
        "curve": _make_pitch_curve(midi_float, conf, times, start_time, end_time),
        "syllable_start": bool(syllable_start),
    }


def _merge_notes(
    midi: np.ndarray,
    midi_float: np.ndarray,
    f0: np.ndarray,
    conf: np.ndarray,
    times: np.ndarray,
    hop_sec: float,
    min_note_sec: float,
    merge_gap_sec: float,
) -> list[dict]:
    notes: list[dict] = []
    current = -1
    start_time = 0.0
    last_time = 0.0
    max_conf = 0.0
    pitches: list[int] = []
    freqs: list[float] = []

    def commit(end_time: float) -> None:
        nonlocal current, start_time, max_conf, pitches, freqs
        duration = end_time - start_time
        if current < 0 or duration < min_note_sec or not pitches:
            return

        values, counts = np.unique(np.asarray(pitches), return_counts=True)
        dominant = int(values[int(np.argmax(counts))])
        notes.append(
            _decorate_note(
                pitch=dominant,
                start_time=float(start_time),
                end_time=float(end_time),
                confidence=float(max_conf),
                midi_float=midi_float,
                f0=f0,
                conf=conf,
                times=times,
            )
        )

    for idx, pitch in enumerate(midi):
        pitched = pitch >= 0
        time_sec = float(times[idx])
        same_pitch = pitched and current >= 0 and abs(int(pitch) - current) <= 1
        gap_ok = (time_sec - last_time) <= merge_gap_sec + hop_sec

        if same_pitch and gap_ok:
            last_time = time_sec
            max_conf = max(max_conf, float(conf[idx]))
            pitches.append(int(pitch))
            if np.isfinite(f0[idx]) and f0[idx] > 0.0:
                freqs.append(float(f0[idx]))
            continue

        if current >= 0:
            commit(last_time + hop_sec)

        if pitched:
            current = int(pitch)
            start_time = time_sec
            last_time = time_sec
            max_conf = float(conf[idx])
            pitches = [int(pitch)]
            freqs = [float(f0[idx])] if np.isfinite(f0[idx]) and f0[idx] > 0.0 else []
        else:
            current = -1
            pitches = []
            freqs = []

    if current >= 0 and len(times):
        commit(float(times[-1]) + hop_sec)

    return notes


def _representative_pitch(midi_values: np.ndarray, confidence: np.ndarray) -> float | None:
    valid = np.isfinite(midi_values) & (midi_values >= 0.0) & (midi_values <= 127.0)
    if not np.any(valid):
        return None

    values = midi_values[valid].astype(np.float32)
    weights = np.clip(confidence[valid].astype(np.float32), 0.05, 1.0) ** 1.4
    if values.size == 0:
        return None

    bin_width = 0.25
    bins = np.floor(values / bin_width).astype(np.int32)
    unique_bins = np.unique(bins)
    best_bin = int(unique_bins[0])
    best_weight = -1.0
    for bin_index in unique_bins:
        total = float(np.sum(weights[np.abs(bins - bin_index) <= 1]))
        if total > best_weight:
            best_weight = total
            best_bin = int(bin_index)

    bin_center = (best_bin + 0.5) * bin_width
    cluster = np.abs(values - bin_center) <= 0.58
    if np.count_nonzero(cluster) >= 2 and float(np.sum(weights[cluster])) >= float(np.sum(weights)) * 0.30:
        values = values[cluster]
        weights = weights[cluster]

    order = np.argsort(values)
    sorted_values = values[order]
    sorted_weights = weights[order]
    cumulative = np.cumsum(sorted_weights)
    midpoint = float(cumulative[-1]) * 0.5
    weighted_median = float(sorted_values[int(np.searchsorted(cumulative, midpoint))])

    spread = float(np.percentile(sorted_values, 90) - np.percentile(sorted_values, 10)) if sorted_values.size >= 4 else 0.0
    if spread >= 1.35:
        weighted_mean = float(np.average(values, weights=weights))
        representative = 0.65 * weighted_median + 0.35 * weighted_mean
    else:
        representative = weighted_median

    return float(np.clip(representative, 0.0, 127.0))


def _rebuild_note(
    note: dict,
    midi_float: np.ndarray,
    f0: np.ndarray,
    conf: np.ndarray,
    times: np.ndarray,
) -> dict:
    start_time = float(note["start"])
    end_time = start_time + float(note["duration"])
    mask = np.isfinite(midi_float) & (times >= start_time - 1.0e-6) & (times <= end_time + 1.0e-6)

    pitch = int(note["pitch"])
    if np.any(mask):
        representative = _representative_pitch(midi_float[mask], conf[mask])
        if representative is not None:
            pitch = int(np.clip(np.rint(representative), 0, 127))

    return _decorate_note(
        pitch=pitch,
        start_time=start_time,
        end_time=end_time,
        confidence=float(note.get("confidence", 0.0)),
        midi_float=midi_float,
        f0=f0,
        conf=conf,
        times=times,
        syllable_start=bool(note.get("syllable_start", False)),
    )


def _merge_pair(
    first: dict,
    second: dict,
    midi_float: np.ndarray,
    f0: np.ndarray,
    conf: np.ndarray,
    times: np.ndarray,
) -> dict:
    start_time = float(first["start"])
    end_time = max(
        float(first["start"]) + float(first["duration"]),
        float(second["start"]) + float(second["duration"]),
    )
    merged = {
        "pitch": first["pitch"],
        "start": start_time,
        "duration": end_time - start_time,
        "confidence": max(float(first.get("confidence", 0.0)), float(second.get("confidence", 0.0))),
    }
    return _rebuild_note(merged, midi_float, f0, conf, times)


def _note_end(note: dict) -> float:
    return float(note["start"]) + float(note["duration"])


def _note_support(
    note: dict,
    midi_float: np.ndarray,
    conf: np.ndarray,
    times: np.ndarray,
) -> dict:
    start_time = float(note["start"])
    end_time = _note_end(note)
    pitch = float(note.get("pitch_exact", note.get("pitch", 60)))
    mask = np.isfinite(midi_float) & (times >= start_time - 1.0e-6) & (times <= end_time + 1.0e-6)
    if not np.any(mask):
        return {"frames": 0, "strong_frames": 0, "weight": 0.0, "near_ratio": 0.0, "median_conf": 0.0}

    local_midi = midi_float[mask]
    local_conf = np.clip(conf[mask], 0.0, 1.0)
    voiced = np.isfinite(local_midi)
    if not np.any(voiced):
        return {"frames": 0, "strong_frames": 0, "weight": 0.0, "near_ratio": 0.0, "median_conf": 0.0}

    local_midi = local_midi[voiced]
    local_conf = local_conf[voiced]
    near = np.abs(local_midi - pitch) <= 0.75
    strong = local_conf >= 0.42
    weight = float(np.sum(local_conf[near]))
    return {
        "frames": int(len(local_midi)),
        "strong_frames": int(np.count_nonzero(strong & near)),
        "weight": weight,
        "near_ratio": float(np.count_nonzero(near)) / float(len(local_midi)),
        "median_conf": float(np.median(local_conf)),
    }


def _is_boundary_protected(note: dict) -> bool:
    return bool(note.get("syllable_start", False)) or str(note.get("source", "")).startswith("gtsinger")


def _suppress_parasitic_notes(
    notes: list[dict],
    midi_float: np.ndarray,
    f0: np.ndarray,
    conf: np.ndarray,
    times: np.ndarray,
) -> list[dict]:
    if len(notes) < 2:
        return notes

    max_gap_sec = 0.075
    bridge_gap_sec = 0.11
    very_short_sec = 0.115
    very_short_slide_sec = 0.095
    short_sec = 0.165

    changed = True
    output = notes
    while changed and len(output) >= 2:
        changed = False
        merged: list[dict] = []
        i = 0
        while i < len(output):
            current = output[i]
            current_support = _note_support(current, midi_float, conf, times)
            current_duration = float(current["duration"])
            protected = _is_boundary_protected(current)
            previous = merged[-1] if merged else None
            nxt = output[i + 1] if i + 1 < len(output) else None

            weak = (
                current_support["strong_frames"] < 3
                or current_support["weight"] < 1.25
                or current_support["near_ratio"] < 0.42
                or current_support["median_conf"] < 0.30
            )
            very_short_weak = current_duration <= very_short_sec and weak
            short_low_support = current_duration <= short_sec and current_support["strong_frames"] <= 4 and current_support["near_ratio"] < 0.58

            if not protected and current_duration <= very_short_sec and previous is not None and nxt is not None:
                previous_gap = float(current["start"]) - _note_end(previous)
                next_gap = float(nxt["start"]) - _note_end(current)
                neighbours_align = abs(int(previous["pitch"]) - int(nxt["pitch"])) <= 1
                current_far = min(abs(int(current["pitch"]) - int(previous["pitch"])),
                                  abs(int(current["pitch"]) - int(nxt["pitch"]))) >= 2
                if previous_gap <= bridge_gap_sec and next_gap <= bridge_gap_sec and neighbours_align and current_far:
                    merged[-1] = _merge_pair(_merge_pair(previous, current, midi_float, f0, conf, times),
                                             nxt, midi_float, f0, conf, times)
                    i += 2
                    changed = True
                    continue

            if not protected and current_duration <= very_short_slide_sec:
                if previous is not None:
                    previous_gap = float(current["start"]) - _note_end(previous)
                    same_family = abs(int(previous["pitch"]) - int(current["pitch"])) <= 3
                    if previous_gap <= max_gap_sec and same_family and float(previous["duration"]) >= current_duration * 2.5:
                        merged[-1] = _merge_pair(previous, current, midi_float, f0, conf, times)
                        i += 1
                        changed = True
                        continue

                if nxt is not None:
                    next_gap = float(nxt["start"]) - _note_end(current)
                    same_family = abs(int(nxt["pitch"]) - int(current["pitch"])) <= 3
                    if next_gap <= max_gap_sec and same_family and float(nxt["duration"]) >= current_duration * 2.5:
                        merged.append(_merge_pair(current, nxt, midi_float, f0, conf, times))
                        i += 2
                        changed = True
                        continue

            if not protected and (very_short_weak or short_low_support):
                if previous is not None and nxt is not None:
                    previous_gap = float(current["start"]) - _note_end(previous)
                    next_gap = float(nxt["start"]) - _note_end(current)
                    neighbours_align = abs(int(previous["pitch"]) - int(nxt["pitch"])) <= 1
                    current_far = min(abs(int(current["pitch"]) - int(previous["pitch"])),
                                      abs(int(current["pitch"]) - int(nxt["pitch"]))) >= 2
                    if previous_gap <= max_gap_sec and next_gap <= max_gap_sec and neighbours_align and current_far:
                        merged[-1] = _merge_pair(_merge_pair(previous, current, midi_float, f0, conf, times),
                                                 nxt, midi_float, f0, conf, times)
                        i += 2
                        changed = True
                        continue

                if previous is not None:
                    previous_gap = float(current["start"]) - _note_end(previous)
                    previous_duration = float(previous["duration"])
                    same_family = abs(int(previous["pitch"]) - int(current["pitch"])) <= 3
                    if previous_gap <= max_gap_sec and same_family and previous_duration >= current_duration * 1.25:
                        merged[-1] = _merge_pair(previous, current, midi_float, f0, conf, times)
                        i += 1
                        changed = True
                        continue

                if nxt is not None:
                    next_gap = float(nxt["start"]) - _note_end(current)
                    next_duration = float(nxt["duration"])
                    same_family = abs(int(nxt["pitch"]) - int(current["pitch"])) <= 3
                    if next_gap <= max_gap_sec and same_family and next_duration >= current_duration * 1.25:
                        merged.append(_merge_pair(current, nxt, midi_float, f0, conf, times))
                        i += 2
                        changed = True
                        continue

            merged.append(_rebuild_note(current, midi_float, f0, conf, times))
            i += 1

        output = merged

    return output


def _merge_ornamental_notes(
    notes: list[dict],
    midi_float: np.ndarray,
    f0: np.ndarray,
    conf: np.ndarray,
    times: np.ndarray,
) -> list[dict]:
    if len(notes) < 2:
        return notes

    max_gap_sec = 0.08
    short_note_sec = 0.18
    slide_note_sec = 0.14

    merged: list[dict] = []
    i = 0
    while i < len(notes):
        current = notes[i]
        current_end = float(current["start"]) + float(current["duration"])

        if i + 2 < len(notes):
            middle = notes[i + 1]
            nxt = notes[i + 2]
            middle_end = float(middle["start"]) + float(middle["duration"])
            next_gap = float(nxt["start"]) - middle_end

            if (
                float(middle["duration"]) <= short_note_sec
                and float(middle["start"]) - current_end <= max_gap_sec
                and next_gap <= max_gap_sec
                and abs(int(current["pitch"]) - int(nxt["pitch"])) <= 1
            ):
                merged.append(_merge_pair(_merge_pair(current, middle, midi_float, f0, conf, times),
                                          nxt, midi_float, f0, conf, times))
                i += 3
                continue

        if i + 1 < len(notes):
            nxt = notes[i + 1]
            gap = float(nxt["start"]) - current_end
            same_family = abs(int(current["pitch"]) - int(nxt["pitch"])) <= 3

            if gap <= max_gap_sec and same_family:
                if float(current["duration"]) <= slide_note_sec < float(nxt["duration"]):
                    merged.append(_merge_pair(current, nxt, midi_float, f0, conf, times))
                    i += 2
                    continue
                if float(nxt["duration"]) <= slide_note_sec < float(current["duration"]):
                    merged.append(_merge_pair(current, nxt, midi_float, f0, conf, times))
                    i += 2
                    continue

        merged.append(_rebuild_note(current, midi_float, f0, conf, times))
        i += 1

    return _suppress_parasitic_notes(merged, midi_float, f0, conf, times)


def _split_notes_on_syllables(
    notes: list[dict],
    syllable_boundaries: np.ndarray,
    midi_float: np.ndarray,
    f0: np.ndarray,
    conf: np.ndarray,
    times: np.ndarray,
) -> list[dict]:
    if len(notes) == 0 or len(syllable_boundaries) == 0:
        return notes

    min_segment_sec = 0.105
    min_note_for_split_sec = 0.26
    split_merge_distance_sec = 0.15

    output: list[dict] = []
    for note in notes:
        start_time = float(note["start"])
        end_time = start_time + float(note["duration"])
        if end_time - start_time < min_note_for_split_sec:
            output.append(note)
            continue

        internal = syllable_boundaries[
            (syllable_boundaries > start_time + min_segment_sec)
            & (syllable_boundaries < end_time - min_segment_sec)
        ]
        if len(internal) == 0:
            output.append(note)
            continue

        split_points: list[float] = []
        for boundary in internal:
            boundary = float(boundary)
            if split_points and boundary - split_points[-1] < split_merge_distance_sec:
                continue
            split_points.append(boundary)

        if not split_points:
            output.append(note)
            continue

        cursor = start_time
        for boundary in split_points + [end_time]:
            if boundary - cursor >= min_segment_sec:
                segment = {
                    "pitch": note["pitch"],
                    "start": cursor,
                    "duration": boundary - cursor,
                    "confidence": note.get("confidence", 0.0),
                    "syllable_start": cursor > start_time + 1.0e-6,
                }
                output.append(_rebuild_note(segment, midi_float, f0, conf, times))
            cursor = boundary

    return output


def _attach_lyrics_to_notes(notes: list[dict], words: list[dict]) -> list[dict]:
    if not notes or not words:
        return notes

    for word in words:
        word_start = float(word["start"])
        word_end = float(word["end"])
        best_index = None
        best_score = -1.0

        for idx, note in enumerate(notes):
            note_start = float(note["start"])
            note_end = note_start + float(note["duration"])
            overlap = max(0.0, min(note_end, word_end) - max(note_start, word_start))
            distance = abs(note_start - word_start)
            score = overlap - 0.15 * distance
            if score > best_score:
                best_score = score
                best_index = idx

        if best_index is None or best_score <= -0.25:
            continue

        existing = str(notes[best_index].get("lyric", "")).strip()
        word_text = str(word["text"]).strip()
        if not word_text:
            continue
        notes[best_index]["lyric"] = (existing + " " + word_text).strip()[:32]

    return notes


def _compute_pitch_tracks(path: Path, sensitivity: float) -> dict:
    sample_rate = 22050
    hop_length = 256
    sensitivity = float(np.clip(sensitivity, 0.0, 1.0))
    confidence_threshold = 0.10 - 0.07 * sensitivity
    fill_gap_sec = 0.35 + 0.40 * sensitivity

    y, sr = librosa.load(path, sr=sample_rate, mono=True, duration=120.0)
    if y.size == 0:
        return {"empty": True}

    f0, voiced_flag, voiced_prob = librosa.pyin(
        y,
        fmin=librosa.note_to_hz("C2"),
        fmax=librosa.note_to_hz("C6"),
        sr=sr,
        frame_length=2048,
        hop_length=hop_length,
        fill_na=np.nan,
    )

    times = librosa.frames_to_time(np.arange(len(f0)), sr=sr, hop_length=hop_length)
    conf = np.nan_to_num(voiced_prob, nan=0.0).astype(np.float32)
    midi = np.full(len(f0), -1, dtype=np.int32)
    valid = np.isfinite(f0) & voiced_flag & (conf >= confidence_threshold)
    midi[valid] = np.rint(librosa.hz_to_midi(f0[valid])).astype(np.int32)
    midi_float = np.full(len(f0), np.nan, dtype=np.float32)
    midi_float[valid] = librosa.hz_to_midi(f0[valid]).astype(np.float32)

    hop_sec = hop_length / sr
    if os.environ.get("SO_DISABLE_CREPE", "0") != "1":
        crepe_midi, crepe_conf = _compute_crepe_track(y, sr, times, hop_sec, sensitivity)
        midi_float, conf = _combine_pitch_tracks(midi_float, conf, crepe_midi, crepe_conf)
        midi = np.full(len(f0), -1, dtype=np.int32)
        valid = np.isfinite(midi_float) & (conf >= confidence_threshold)
        midi[valid] = np.rint(midi_float[valid]).astype(np.int32)

    midi = _fill_short_gaps(midi, conf, max_gap_frames=int(round(fill_gap_sec / hop_sec)))
    midi = _median_smooth(midi, radius=4)
    return {
        "empty": False,
        "y": y,
        "sr": sr,
        "hop_length": hop_length,
        "sensitivity": sensitivity,
        "midi": midi,
        "midi_float": midi_float,
        "f0": np.nan_to_num(f0, nan=0.0),
        "conf": conf,
        "times": times,
        "hop_sec": hop_sec,
    }


def analyze(path: Path, sensitivity: float) -> list[dict]:
    tracks = _compute_pitch_tracks(path, sensitivity)
    if tracks["empty"]:
        return []

    min_note_sec = 0.12 - 0.07 * float(tracks["sensitivity"])
    merge_gap_sec = 0.24 + 0.22 * float(tracks["sensitivity"])
    use_syllable_split = os.environ.get("SO_ENABLE_SYLLABLE_SPLIT", "0") == "1"
    syllable_boundaries = (
        _detect_syllable_boundaries(tracks["y"], tracks["sr"], tracks["hop_length"], tracks["sensitivity"])
        if use_syllable_split
        else np.asarray([], dtype=np.float32)
    )

    notes = _merge_notes(
        midi=tracks["midi"],
        midi_float=tracks["midi_float"],
        f0=tracks["f0"],
        conf=tracks["conf"],
        times=tracks["times"],
        hop_sec=tracks["hop_sec"],
        min_note_sec=min_note_sec,
        merge_gap_sec=merge_gap_sec,
    )
    notes = _merge_ornamental_notes(notes, tracks["midi_float"], tracks["f0"], tracks["conf"], tracks["times"])
    if use_syllable_split:
        notes = _split_notes_on_syllables(notes, syllable_boundaries, tracks["midi_float"], tracks["f0"], tracks["conf"], tracks["times"])
    return _attach_lyrics_to_notes(notes, _detect_lyrics(path))


def _parse_region(value: str) -> tuple[str, float, float, int]:
    parts = value.split(":")
    if len(parts) not in (3, 4):
        raise argparse.ArgumentTypeError("region must be id:start:end[:pitch]")

    note_id = parts[0]
    start = float(parts[1])
    end = float(parts[2])
    pitch = int(parts[3]) if len(parts) == 4 else 60
    if not note_id or end <= start:
        raise argparse.ArgumentTypeError("region has invalid id/start/end")

    return note_id, start, end, int(np.clip(pitch, 0, 127))


def analyze_regions(path: Path, sensitivity: float, regions: list[tuple[str, float, float, int]]) -> list[dict]:
    tracks = _compute_pitch_tracks(path, sensitivity)
    if tracks["empty"]:
        return []

    notes: list[dict] = []
    for note_id, start_time, end_time, pitch in regions:
        mask = (
            np.isfinite(tracks["midi_float"])
            & (tracks["times"] >= start_time - 1.0e-6)
            & (tracks["times"] <= end_time + 1.0e-6)
        )
        confidence = float(np.max(tracks["conf"][mask])) if np.any(mask) else 0.0
        note = _rebuild_note(
            {
                "pitch": pitch,
                "start": start_time,
                "duration": end_time - start_time,
                "confidence": confidence,
            },
            tracks["midi_float"],
            tracks["f0"],
            tracks["conf"],
            tracks["times"],
        )
        note["id"] = note_id
        notes.append(note)

    return notes


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("audio_file", type=Path)
    parser.add_argument("--sensitivity", type=float, default=0.72)
    parser.add_argument("--region", action="append", type=_parse_region, default=[])
    args = parser.parse_args()
    notes = analyze_regions(args.audio_file, args.sensitivity, args.region) if args.region else analyze(args.audio_file, args.sensitivity)
    print(json.dumps({"notes": notes}, ensure_ascii=False))


if __name__ == "__main__":
    main()
