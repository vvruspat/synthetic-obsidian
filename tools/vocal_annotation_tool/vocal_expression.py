#!/usr/bin/env python3
"""Singer-specific F0 expression analysis and backing-vocal curve generation."""

from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
from typing import Any, Iterable

import numpy as np
from scipy.ndimage import gaussian_filter1d


DEFAULT_F0_RATE = 50.0
LOCAL_EXPRESSION_THRESHOLD_CENTS = 16.0
SUSTAINED_NOTE_VIBRATO_SECONDS = 0.85
MIN_AUDIBLE_VIBRATO_DEPTH_CENTS = 38.0
CONNECTED_NOTE_GAP_SECONDS = 0.06
ATTACK_SETTLE_SECONDS = 0.09


def default_expression_profile() -> dict[str, Any]:
    """Return a neutral profile used when singer analysis is inconclusive."""
    return {
        "schema_version": 1,
        "vibrato": {
            "rate_hz": 5.6,
            "rate_std_hz": 0.25,
            "depth_cents": 28.0,
            "depth_std_cents": 7.0,
            "onset_delay_seconds": 0.24,
            "attack_seconds": 0.18,
            "release_seconds": 0.12,
            "minimum_note_seconds": 0.48,
            "usage_probability": 0.65,
            "sample_count": 0,
            "confidence": 0.0,
        },
        "legato": {
            "duration_seconds": 0.085,
            "duration_std_seconds": 0.018,
            "median_interval_semitones": 2.0,
            "midpoint": 0.5,
            "overshoot_cents": 0.0,
            "sample_count": 0,
            "confidence": 0.0,
        },
        "analysis": {
            "f0_rate_hz": DEFAULT_F0_RATE,
            "contour_count": 0,
            "voiced_seconds": 0.0,
        },
    }


def _finite_float(
    value: Any,
    fallback: float,
    minimum: float,
    maximum: float,
) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return fallback
    if not math.isfinite(result):
        return fallback
    return float(np.clip(result, minimum, maximum))


def _finite_int(value: Any, fallback: int = 0) -> int:
    try:
        return max(0, int(value))
    except (TypeError, ValueError):
        return fallback


_PROFILE_BOUNDS = {
    "vibrato": {
        "rate_hz": (3.5, 9.0),
        "rate_std_hz": (0.0, 2.0),
        "depth_cents": (0.0, 120.0),
        "depth_std_cents": (0.0, 60.0),
        "onset_delay_seconds": (0.0, 1.0),
        "attack_seconds": (0.02, 0.8),
        "release_seconds": (0.02, 0.8),
        "minimum_note_seconds": (0.2, 2.0),
        "usage_probability": (0.0, 1.0),
        "confidence": (0.0, 1.0),
    },
    "legato": {
        "duration_seconds": (0.025, 0.35),
        "duration_std_seconds": (0.0, 0.15),
        "median_interval_semitones": (0.5, 24.0),
        "midpoint": (0.2, 0.8),
        "overshoot_cents": (0.0, 80.0),
        "confidence": (0.0, 1.0),
    },
    "analysis": {
        "f0_rate_hz": (10.0, 400.0),
        "voiced_seconds": (0.0, 1_000_000.0),
    },
}


def normalize_expression_profile(value: Any) -> dict[str, Any]:
    """Validate a profile and fill missing fields without trusting persisted data."""
    if not isinstance(value, dict):
        value = {}
    profile = default_expression_profile()
    for section_name, bounds in _PROFILE_BOUNDS.items():
        source = value.get(section_name)
        if not isinstance(source, dict):
            source = {}
        target = profile[section_name]
        for name, (minimum, maximum) in bounds.items():
            target[name] = _finite_float(
                source.get(name),
                target[name],
                minimum,
                maximum,
            )
        if "sample_count" in target:
            target["sample_count"] = _finite_int(source.get("sample_count"))
        if "contour_count" in target:
            target["contour_count"] = _finite_int(source.get("contour_count"))
    return profile


def midi_to_hz(midi: np.ndarray | float) -> np.ndarray | float:
    return 440.0 * np.power(2.0, (np.asarray(midi) - 69.0) / 12.0)


def hz_to_midi(hz: np.ndarray) -> np.ndarray:
    return 69.0 + 12.0 * np.log2(np.maximum(hz, 1e-8) / 440.0)


def _fill_short_gaps(mask: np.ndarray, maximum_gap: int) -> np.ndarray:
    result = np.asarray(mask, dtype=bool).copy()
    if maximum_gap <= 0 or not np.any(result):
        return result
    indices = np.flatnonzero(result)
    for left, right in zip(indices, indices[1:]):
        if 1 < right - left <= maximum_gap + 1:
            result[left : right + 1] = True
    return result


def _true_runs(mask: np.ndarray, minimum_frames: int) -> list[tuple[int, int]]:
    padded = np.pad(np.asarray(mask, dtype=np.int8), (1, 1))
    changes = np.diff(padded)
    starts = np.flatnonzero(changes == 1)
    ends = np.flatnonzero(changes == -1)
    return [
        (int(start), int(end))
        for start, end in zip(starts, ends)
        if end - start >= minimum_frames
    ]


def _robust_std(values: list[float], fallback: float) -> float:
    if len(values) < 2:
        return fallback
    array = np.asarray(values, dtype=np.float64)
    median = float(np.median(array))
    return float(1.4826 * np.median(np.abs(array - median)))


def _analyse_vibrato(
    voiced_runs: list[np.ndarray],
    f0_rate: float,
) -> tuple[list[float], list[float], list[float], int]:
    rates: list[float] = []
    depths: list[float] = []
    confidences: list[float] = []
    eligible_windows = 0
    window_frames = max(32, int(round(1.0 * f0_rate)))
    hop_frames = max(4, int(round(0.25 * f0_rate)))
    frequencies = np.arange(4.0, 8.51, 0.05, dtype=np.float64)

    for midi in voiced_runs:
        if len(midi) < window_frames:
            continue
        sigma = max(1.0, 0.10 * f0_rate)
        center = gaussian_filter1d(midi, sigma=sigma, mode="nearest")
        residual = midi - center

        for start in range(0, len(midi) - window_frames + 1, hop_frames):
            end = start + window_frames
            center_window = center[start:end]
            if (
                np.percentile(center_window, 90) - np.percentile(center_window, 10)
                > 0.9
            ):
                continue
            eligible_windows += 1

            values = residual[start:end].astype(np.float64)
            values -= np.mean(values)
            variance = float(np.mean(np.square(values)))
            if variance < 1e-6:
                continue
            times = np.arange(window_frames, dtype=np.float64) / f0_rate
            best: tuple[float, float, float] | None = None
            for frequency in frequencies:
                design = np.column_stack(
                    (
                        np.sin(2.0 * np.pi * frequency * times),
                        np.cos(2.0 * np.pi * frequency * times),
                    )
                )
                coefficients, _, _, _ = np.linalg.lstsq(
                    design,
                    values,
                    rcond=None,
                )
                fitted = design @ coefficients
                residual_variance = float(np.mean(np.square(values - fitted)))
                explained = 1.0 - residual_variance / variance
                amplitude_cents = (
                    float(np.hypot(coefficients[0], coefficients[1])) * 100.0
                )
                if best is None or explained > best[2]:
                    best = (float(frequency), amplitude_cents, explained)

            if best is None:
                continue
            frequency, amplitude_cents, explained = best
            if not 6.0 <= amplitude_cents <= 120.0 or explained < 0.38:
                continue
            rates.append(frequency)
            depths.append(amplitude_cents)
            confidences.append(explained)

    return rates, depths, confidences, eligible_windows


def _analyse_legato(
    voiced_runs: list[np.ndarray],
    f0_rate: float,
) -> tuple[list[float], list[float], list[float], list[float]]:
    durations: list[float] = []
    intervals: list[float] = []
    midpoints: list[float] = []
    overshoots: list[float] = []
    context_frames = max(3, int(round(0.10 * f0_rate)))

    for midi in voiced_runs:
        if len(midi) < context_frames * 3:
            continue
        smooth = gaussian_filter1d(
            midi,
            sigma=max(1.0, 0.07 * f0_rate),
            mode="nearest",
        )
        velocity = np.gradient(smooth) * f0_rate
        motion = _fill_short_gaps(np.abs(velocity) >= 2.5, 2)
        for start, end in _true_runs(motion, minimum_frames=2):
            if start < context_frames or end + context_frames > len(smooth):
                continue
            duration = (end - start) / f0_rate
            if not 0.035 <= duration <= 0.35:
                continue

            left_pitch = float(np.median(smooth[start - context_frames : start]))
            right_pitch = float(np.median(smooth[end : end + context_frames]))
            interval = right_pitch - left_pitch
            if not 0.5 <= abs(interval) <= 24.0:
                continue

            transition = smooth[start:end]
            progress = np.clip(
                (transition - left_pitch) / interval,
                0.0,
                1.0,
            )
            crossing = np.flatnonzero(progress >= 0.5)
            midpoint = (
                float(crossing[0]) / max(1, len(progress) - 1) if len(crossing) else 0.5
            )
            direction = 1.0 if interval > 0.0 else -1.0
            settling = smooth[end : end + context_frames]
            overshoot = max(
                0.0,
                float(np.max((settling - right_pitch) * direction) * 100.0),
            )

            durations.append(duration)
            intervals.append(abs(interval))
            midpoints.append(float(np.clip(midpoint, 0.2, 0.8)))
            overshoots.append(float(np.clip(overshoot, 0.0, 80.0)))

    return durations, intervals, midpoints, overshoots


def analyze_expression_profile(
    f0_contours: Iterable[np.ndarray],
    *,
    f0_rate: float = DEFAULT_F0_RATE,
) -> dict[str, Any]:
    """Estimate robust singer-level vibrato and legato statistics from F0."""
    defaults = default_expression_profile()
    voiced_runs: list[np.ndarray] = []
    contour_count = 0
    voiced_frames = 0
    minimum_run_frames = max(8, int(round(0.35 * f0_rate)))

    for value in f0_contours:
        contour = np.asarray(value, dtype=np.float64).reshape(-1)
        if not len(contour):
            continue
        contour_count += 1
        voiced = np.isfinite(contour) & (contour > 32.0)
        voiced = _fill_short_gaps(voiced, max(1, int(round(0.04 * f0_rate))))
        voiced_frames += int(np.count_nonzero(voiced))
        for start, end in _true_runs(voiced, minimum_run_frames):
            region = contour[start:end]
            valid = np.isfinite(region) & (region > 32.0)
            if np.count_nonzero(valid) < 2:
                continue
            positions = np.arange(len(region), dtype=np.float64)
            filled = np.interp(positions, positions[valid], region[valid])
            voiced_runs.append(hz_to_midi(filled))

    rates, depths, vibrato_confidences, eligible_vibrato_windows = _analyse_vibrato(
        voiced_runs,
        f0_rate,
    )
    durations, intervals, midpoints, overshoots = _analyse_legato(
        voiced_runs,
        f0_rate,
    )

    vibrato = dict(defaults["vibrato"])
    if rates:
        observation_confidence = float(np.median(vibrato_confidences))
        coverage_confidence = min(1.0, len(rates) / 12.0)
        vibrato.update(
            {
                "rate_hz": float(np.median(rates)),
                "rate_std_hz": _robust_std(
                    rates,
                    vibrato["rate_std_hz"],
                ),
                "depth_cents": float(np.median(depths)),
                "depth_std_cents": _robust_std(
                    depths,
                    vibrato["depth_std_cents"],
                ),
                "usage_probability": float(
                    np.clip(
                        len(rates) / max(1, eligible_vibrato_windows),
                        0.0,
                        1.0,
                    )
                ),
                "sample_count": len(rates),
                "confidence": float(
                    np.clip(
                        observation_confidence * coverage_confidence,
                        0.0,
                        1.0,
                    )
                ),
            }
        )
    elif eligible_vibrato_windows >= 8:
        vibrato.update(
            {
                "usage_probability": 0.0,
                "confidence": min(0.8, eligible_vibrato_windows / 30.0),
            }
        )

    legato = dict(defaults["legato"])
    if durations:
        legato.update(
            {
                "duration_seconds": float(np.median(durations)),
                "duration_std_seconds": _robust_std(
                    durations,
                    legato["duration_std_seconds"],
                ),
                "median_interval_semitones": float(np.median(intervals)),
                "midpoint": float(np.median(midpoints)),
                "overshoot_cents": float(np.median(overshoots)),
                "sample_count": len(durations),
                "confidence": min(1.0, len(durations) / 10.0),
            }
        )

    return normalize_expression_profile(
        {
            "schema_version": 1,
            "vibrato": vibrato,
            "legato": legato,
            "analysis": {
                "f0_rate_hz": f0_rate,
                "contour_count": contour_count,
                "voiced_seconds": voiced_frames / max(f0_rate, 1e-6),
            },
        }
    )


def expression_profile_path_for_adapter(adapter_path: Path) -> Path:
    return adapter_path.with_name("expression-profile.json")


def load_expression_profile_for_adapter(
    adapter_path: str | Path | None,
) -> tuple[dict[str, Any] | None, Path | None]:
    if not adapter_path:
        return None, None
    adapter = Path(adapter_path).expanduser().resolve()
    candidates = [
        expression_profile_path_for_adapter(adapter),
        adapter.parent.parent / "expression-profile.json",
    ]
    for candidate in candidates:
        if not candidate.is_file():
            continue
        try:
            payload = json.loads(candidate.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        return normalize_expression_profile(payload), candidate
    return None, None


def note_pitch_at(note: dict[str, Any], time_sec: float) -> float:
    curve = note.get("curve")
    if not isinstance(curve, list) or not curve:
        return float(note.get("pitch_exact", note.get("pitch", 60.0)))

    points: list[tuple[float, float]] = []
    for value in curve:
        if not isinstance(value, dict):
            continue
        try:
            points.append((float(value["time"]), float(value["midi"])))
        except (KeyError, TypeError, ValueError):
            continue
    if not points:
        return float(note.get("pitch_exact", note.get("pitch", 60.0)))

    points.sort(key=lambda item: item[0])
    if time_sec <= points[0][0]:
        return points[0][1]
    if time_sec >= points[-1][0]:
        return points[-1][1]
    for (left_time, left_midi), (right_time, right_midi) in zip(
        points,
        points[1:],
    ):
        if left_time <= time_sec <= right_time:
            alpha = (time_sec - left_time) / max(1e-6, right_time - left_time)
            return left_midi + (right_midi - left_midi) * alpha
    return points[-1][1]


def _blend_with_default(
    measured: float,
    fallback: float,
    confidence: float,
) -> float:
    return fallback + (measured - fallback) * float(np.clip(confidence, 0.0, 1.0))


def _smoothstep(value: np.ndarray) -> np.ndarray:
    clipped = np.clip(value, 0.0, 1.0)
    return clipped * clipped * (3.0 - 2.0 * clipped)


def _stable_seed(notes: list[dict[str, Any]]) -> int:
    parts = []
    for note in notes:
        parts.append(
            "|".join(
                (
                    str(note.get("id", "")),
                    str(note.get("start", "")),
                    str(note.get("end", "")),
                    str(note.get("pitch_exact", note.get("pitch", ""))),
                )
            )
        )
    digest = hashlib.sha256("\n".join(parts).encode("utf-8")).digest()
    return int.from_bytes(digest[:8], byteorder="little", signed=False)


def _notes_are_legato(
    previous: dict[str, Any],
    current: dict[str, Any],
) -> bool:
    previous_end = float(previous.get("voiced_end", previous.get("end", 0.0)))
    current_start = float(
        current.get("voiced_start", current.get("start", previous_end))
    )
    same_syllable = bool(current.get("syllable_id")) and (
        str(current.get("syllable_id")) == str(previous.get("syllable_id"))
    )
    requested = bool(current.get("legato_from_previous")) or same_syllable
    return requested and -0.02 <= current_start - previous_end <= 0.08


def _notes_are_connected(
    previous: dict[str, Any],
    current: dict[str, Any],
) -> bool:
    previous_end = float(previous.get("voiced_end", previous.get("end", 0.0)))
    current_start = float(
        current.get("voiced_start", current.get("start", previous_end))
    )
    return -0.02 <= current_start - previous_end <= CONNECTED_NOTE_GAP_SECONDS


def _curve_has_local_expression(
    note: dict[str, Any],
    base_midi: np.ndarray,
    f0_rate: float,
) -> bool:
    curve = note.get("curve")
    if not isinstance(curve, list) or len(curve) < 4 or len(base_midi) < 8:
        return False
    center = gaussian_filter1d(
        base_midi,
        sigma=max(1.0, 0.08 * f0_rate),
        mode="nearest",
    )
    residual_cents = (base_midi - center) * 100.0
    # Generated note curves often contain a few cents of resampling jitter. That
    # is not audible expression and must not suppress the singer's vibrato.
    return (
        float(np.percentile(np.abs(residual_cents), 85))
        >= LOCAL_EXPRESSION_THRESHOLD_CENTS
    )


def _time_warp_for_midpoint(
    progress: np.ndarray,
    midpoint: float,
) -> np.ndarray:
    midpoint = float(np.clip(midpoint, 0.2, 0.8))
    left = 0.5 * progress / midpoint
    right = 0.5 + 0.5 * (progress - midpoint) / (1.0 - midpoint)
    return np.where(progress <= midpoint, left, right)


def build_target_f0(
    notes: list[dict[str, Any]],
    source_f0: np.ndarray,
    expression_profile: dict[str, Any] | None = None,
    *,
    f0_rate: float = DEFAULT_F0_RATE,
    seed: int | None = None,
) -> tuple[np.ndarray, np.ndarray]:
    """Create an expressive F0 curve from backing notes, not from lead pitch."""
    profile = normalize_expression_profile(expression_profile)
    defaults = default_expression_profile()
    ordered_notes = sorted(
        (
            note
            for note in notes
            if float(note.get("voiced_end", note.get("end", 0.0)))
            > float(note.get("voiced_start", note.get("start", 0.0)))
        ),
        key=lambda item: (
            float(item.get("voiced_start", item.get("start", 0.0))),
            float(item.get("voiced_end", item.get("end", 0.0))),
        ),
    )
    source = np.asarray(source_f0, dtype=np.float32).reshape(-1)
    target_midi = np.full(len(source), np.nan, dtype=np.float32)
    note_mask = np.zeros(len(source), dtype=np.float32)
    vibrato_cents_layer = np.zeros(len(source), dtype=np.float32)
    attack_cents_layer = np.zeros(len(source), dtype=np.float32)
    frame_times = np.arange(len(source), dtype=np.float64) / f0_rate
    rng = np.random.default_rng(_stable_seed(ordered_notes) if seed is None else seed)
    note_regions: list[dict[str, Any]] = []

    for note in ordered_notes:
        start = float(note.get("voiced_start", note.get("start", 0.0)))
        end = float(note.get("voiced_end", note.get("end", start)))
        active = (frame_times >= start) & (frame_times < end)
        indices = np.flatnonzero(active)
        if not len(indices):
            continue
        base_midi = np.asarray(
            [note_pitch_at(note, float(frame_times[index])) for index in indices],
            dtype=np.float32,
        )
        target_midi[indices] = base_midi
        note_mask[indices] = 1.0
        note_regions.append(
            {
                "note": note,
                "start": start,
                "end": end,
                "indices": indices,
                "base_midi": base_midi,
            }
        )

    vibrato = profile["vibrato"]
    vibrato_defaults = defaults["vibrato"]
    vibrato_confidence = float(vibrato["confidence"])
    rate_hz = _blend_with_default(
        vibrato["rate_hz"],
        vibrato_defaults["rate_hz"],
        vibrato_confidence,
    )
    rate_std_hz = _blend_with_default(
        vibrato["rate_std_hz"],
        vibrato_defaults["rate_std_hz"],
        vibrato_confidence,
    )
    depth_cents = _blend_with_default(
        vibrato["depth_cents"],
        vibrato_defaults["depth_cents"],
        vibrato_confidence,
    )
    depth_std_cents = _blend_with_default(
        vibrato["depth_std_cents"],
        vibrato_defaults["depth_std_cents"],
        vibrato_confidence,
    )
    onset_delay = _blend_with_default(
        vibrato["onset_delay_seconds"],
        vibrato_defaults["onset_delay_seconds"],
        vibrato_confidence,
    )
    usage_probability = _blend_with_default(
        vibrato["usage_probability"],
        vibrato_defaults["usage_probability"],
        vibrato_confidence,
    )
    phase_at_previous_end: float | None = None

    for index, region in enumerate(note_regions):
        note = region["note"]
        indices = region["indices"]
        base_midi = region["base_midi"]
        duration = region["end"] - region["start"]
        legato_from_previous = index > 0 and _notes_are_legato(
            note_regions[index - 1]["note"], note
        )
        connected_from_previous = index > 0 and _notes_are_connected(
            note_regions[index - 1]["note"], note
        )
        connected_to_next = index + 1 < len(note_regions) and _notes_are_connected(
            note, note_regions[index + 1]["note"]
        )
        phase_start = (
            phase_at_previous_end
            if connected_from_previous and phase_at_previous_end is not None
            else float(rng.uniform(0.0, 2.0 * np.pi))
        )
        sampled_rate = float(
            np.clip(
                rng.normal(rate_hz, rate_std_hz * 0.35),
                3.5,
                9.0,
            )
        )
        relative_times = frame_times[indices] - region["start"]
        jitter_point_count = max(2, int(math.ceil(duration / 0.25)) + 1)
        jitter_points = rng.normal(
            0.0,
            min(0.18, rate_std_hz * 0.20),
            size=jitter_point_count,
        )
        jitter_times = np.linspace(0.0, max(duration, 1e-6), jitter_point_count)
        instantaneous_rate = sampled_rate + np.interp(
            relative_times,
            jitter_times,
            jitter_points,
        )
        phase = phase_start + 2.0 * np.pi * np.cumsum(instantaneous_rate / f0_rate)
        phase_at_previous_end = float(phase[-1]) if len(phase) else phase_start

        minimum_note_seconds = float(vibrato["minimum_note_seconds"])
        sustained_note = duration >= max(
            SUSTAINED_NOTE_VIBRATO_SECONDS,
            minimum_note_seconds * 1.35,
        )
        duration_probability = float(
            np.clip(
                0.65 + (duration - minimum_note_seconds) / 0.55,
                0.65,
                1.0,
            )
        )
        should_add_vibrato = (
            duration >= minimum_note_seconds
            and depth_cents > 0.0
            and usage_probability > 0.0
            and (
                sustained_note
                or rng.random() <= usage_probability * duration_probability
            )
            and not _curve_has_local_expression(note, base_midi, f0_rate)
        )
        if not should_add_vibrato:
            continue

        sampled_depth = float(
            np.clip(
                max(
                    MIN_AUDIBLE_VIBRATO_DEPTH_CENTS,
                    rng.normal(depth_cents, depth_std_cents * 0.35),
                ),
                0.0,
                120.0,
            )
        )
        note_onset_delay = onset_delay * (0.55 if legato_from_previous else 1.0)
        note_onset_delay = min(
            note_onset_delay,
            max(0.0, duration - vibrato["release_seconds"] - 0.08),
        )
        attack = min(
            vibrato["attack_seconds"],
            max(0.02, (duration - note_onset_delay) * 0.4),
        )
        release = min(
            vibrato["release_seconds"],
            max(0.02, duration * 0.3),
        )
        attack_envelope = (
            np.ones_like(relative_times)
            if connected_from_previous
            else _smoothstep(
                (relative_times - note_onset_delay) / max(attack, 1e-6)
            )
        )
        release_envelope = (
            np.ones_like(relative_times)
            if connected_to_next
            else _smoothstep(
                (duration - relative_times) / max(release, 1e-6)
            )
        )
        vibrato_cents = (
            sampled_depth * attack_envelope * release_envelope * np.sin(phase)
        )
        vibrato_cents_layer[indices] = vibrato_cents.astype(np.float32)

    legato = profile["legato"]
    legato_defaults = defaults["legato"]
    legato_confidence = float(legato["confidence"])
    base_transition_duration = _blend_with_default(
        legato["duration_seconds"],
        legato_defaults["duration_seconds"],
        legato_confidence,
    )
    duration_std = _blend_with_default(
        legato["duration_std_seconds"],
        legato_defaults["duration_std_seconds"],
        legato_confidence,
    )
    median_interval = _blend_with_default(
        legato["median_interval_semitones"],
        legato_defaults["median_interval_semitones"],
        legato_confidence,
    )
    midpoint = _blend_with_default(
        legato["midpoint"],
        legato_defaults["midpoint"],
        legato_confidence,
    )
    overshoot_cents = _blend_with_default(
        legato["overshoot_cents"],
        legato_defaults["overshoot_cents"],
        legato_confidence,
    )

    for previous, current in zip(note_regions, note_regions[1:]):
        explicit_legato = _notes_are_legato(previous["note"], current["note"])
        if not explicit_legato and not _notes_are_connected(
            previous["note"], current["note"]
        ):
            continue
        boundary = float(current["start"])
        previous_pitch = note_pitch_at(
            previous["note"],
            min(boundary, previous["end"]),
        )
        current_pitch = note_pitch_at(current["note"], boundary)
        interval = current_pitch - previous_pitch
        if abs(interval) < 0.05:
            continue

        interval_scale = np.clip(
            math.sqrt(abs(interval) / max(median_interval, 0.5)),
            0.72,
            1.55,
        )
        transition_duration = float(
            np.clip(
                rng.normal(
                    base_transition_duration
                    * interval_scale
                    * (1.0 if explicit_legato else 0.68),
                    duration_std * 0.35,
                ),
                0.035,
                min(
                    0.24 if explicit_legato else 0.10,
                    max(0.035, previous["end"] - previous["start"]),
                    max(0.035, current["end"] - current["start"]),
                ),
            )
        )
        transition_start = max(
            previous["start"],
            boundary - transition_duration * 0.45,
        )
        transition_end = min(
            current["end"],
            transition_start + transition_duration,
        )
        transition_indices = np.flatnonzero(
            (frame_times >= transition_start) & (frame_times <= transition_end)
        )
        if len(transition_indices) < 2:
            continue

        progress = (frame_times[transition_indices] - transition_start) / max(
            transition_end - transition_start, 1e-6
        )
        warped = _time_warp_for_midpoint(progress, midpoint)
        alpha = _smoothstep(warped)
        transition_midi = previous_pitch + interval * alpha
        if overshoot_cents > 0.0:
            tail = np.clip((progress - 0.62) / 0.38, 0.0, 1.0)
            transition_midi += (
                np.sign(interval)
                * overshoot_cents
                / 100.0
                * np.sin(np.pi * tail)
                * tail
            )
        target_midi[transition_indices] = transition_midi.astype(np.float32)
        note_mask[transition_indices] = 1.0

    # A sung note rarely locks to the pitch centre on its very first voiced
    # frame. Keep this as an independent layer so it does not alter note or
    # legato geometry. Explicit legato notes retain the continuous transition
    # instead of receiving a new attack.
    for index, region in enumerate(note_regions):
        if index > 0 and _notes_are_connected(
            note_regions[index - 1]["note"], region["note"]
        ):
            continue
        duration = region["end"] - region["start"]
        settle_seconds = min(ATTACK_SETTLE_SECONDS, duration * 0.28)
        if settle_seconds < 0.035:
            continue
        indices = region["indices"]
        relative_times = frame_times[indices] - region["start"]
        active = relative_times <= settle_seconds
        if not np.any(active):
            continue
        attack_depth_cents = float(rng.uniform(16.0, 24.0))
        settle = 1.0 - _smoothstep(
            relative_times[active] / max(settle_seconds, 1e-6)
        )
        attack_cents_layer[indices[active]] += (
            attack_depth_cents * settle
        ).astype(np.float32)

    # Expression is deliberately composed after the musical note/transition
    # contour. This keeps vibrato continuous through legato and lets attack
    # settling be tuned independently from the generated MIDI melody.
    expressed = np.isfinite(target_midi)
    target_midi[expressed] += (
        vibrato_cents_layer[expressed] + attack_cents_layer[expressed]
    ) / 100.0

    target_f0 = np.zeros(len(source), dtype=np.float32)
    voiced_target = np.isfinite(target_midi) & np.isfinite(source) & (source > 32.0)
    target_f0[voiced_target] = np.asarray(
        midi_to_hz(target_midi[voiced_target]),
        dtype=np.float32,
    )
    note_mask[~np.isfinite(target_midi)] = 0.0
    return target_f0, note_mask
