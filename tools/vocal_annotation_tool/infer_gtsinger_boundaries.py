#!/usr/bin/env python3
"""Run the offline GTSinger TCN boundary model on one audio file."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import librosa
import numpy as np
import torch


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("audio", type=Path)
    parser.add_argument("--checkpoint", type=Path, required=True)
    return parser.parse_args()


def clean_interval_masks(
    probabilities: np.ndarray,
    thresholds: tuple[float, float],
    min_frames: int,
    merge_gap_frames: int,
) -> np.ndarray:
    masks = probabilities >= np.asarray(thresholds)[:, None]
    overlap = masks[0] & masks[1]
    if np.any(overlap):
        margins = probabilities[:, overlap] - np.asarray(thresholds)[:, None]
        winner = np.argmax(margins, axis=0)
        masks[0, overlap] = winner == 0
        masks[1, overlap] = winner == 1

    for index in range(masks.shape[0]):
        padded = np.pad(masks[index].astype(np.int8), (1, 1))
        edges = np.diff(padded)
        starts = np.flatnonzero(edges == 1)
        ends = np.flatnonzero(edges == -1)
        for start, end in zip(starts, ends, strict=True):
            if end - start < min_frames:
                masks[index, start:end] = False

        padded = np.pad(masks[index].astype(np.int8), (1, 1))
        edges = np.diff(padded)
        starts = np.flatnonzero(edges == 1)
        ends = np.flatnonzero(edges == -1)
        for previous_end, next_start in zip(ends[:-1], starts[1:], strict=True):
            if next_start - previous_end <= merge_gap_frames:
                masks[index, previous_end:next_start] = True
    return masks


def hysteresis_mask(
    probabilities: np.ndarray,
    seed_threshold: float,
    floor_threshold: float,
) -> np.ndarray:
    """Keep low-threshold regions only when they contain a confident peak."""
    low_mask = probabilities >= floor_threshold
    seed_mask = probabilities >= seed_threshold
    result = np.zeros_like(low_mask, dtype=bool)

    padded = np.pad(low_mask.astype(np.int8), (1, 1))
    edges = np.diff(padded)
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    for start, end in zip(starts, ends, strict=True):
        if np.any(seed_mask[start:end]):
            result[start:end] = True
    return result


def clean_interval_masks_with_breath_hysteresis(
    probabilities: np.ndarray,
    thresholds: tuple[float, float],
    min_frames: int,
    merge_gap_frames: int,
) -> np.ndarray:
    if probabilities.shape[0] != 2:
        raise ValueError("expected breath and silence probabilities")

    breath_seed_threshold = float(thresholds[0])
    breath_floor_threshold = min(0.25, breath_seed_threshold * 0.50)
    masks = np.asarray(
        [
            hysteresis_mask(
                probabilities[0],
                seed_threshold=breath_seed_threshold,
                floor_threshold=breath_floor_threshold,
            ),
            probabilities[1] >= float(thresholds[1]),
        ],
        dtype=bool,
    )

    overlap = masks[0] & masks[1]
    if np.any(overlap):
        # Breath uses a peak-seeded mask. If a breath island was accepted, do
        # not let the broad silence head carve it away in the overlap area.
        masks[1, overlap] = False

    for index in range(masks.shape[0]):
        local_min_frames = max(min_frames, 10) if index == 0 else min_frames
        local_merge_gap_frames = max(merge_gap_frames, 20) if index == 0 else merge_gap_frames

        padded = np.pad(masks[index].astype(np.int8), (1, 1))
        edges = np.diff(padded)
        starts = np.flatnonzero(edges == 1)
        ends = np.flatnonzero(edges == -1)
        for previous_end, next_start in zip(ends[:-1], starts[1:], strict=True):
            if next_start - previous_end <= local_merge_gap_frames:
                masks[index, previous_end:next_start] = True

        padded = np.pad(masks[index].astype(np.int8), (1, 1))
        edges = np.diff(padded)
        starts = np.flatnonzero(edges == 1)
        ends = np.flatnonzero(edges == -1)
        for start, end in zip(starts, ends, strict=True):
            if end - start < local_min_frames:
                masks[index, start:end] = False
    return masks


def mask_to_intervals(
    mask: np.ndarray,
    probabilities: np.ndarray,
    frame_seconds: float,
) -> list[dict[str, float]]:
    padded = np.pad(mask.astype(np.int8), (1, 1))
    edges = np.diff(padded)
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    return [
        {
            "start": round(float(start * frame_seconds), 6),
            "end": round(float(end * frame_seconds), 6),
            "confidence": round(float(np.mean(probabilities[start:end])), 6),
        }
        for start, end in zip(starts, ends, strict=True)
        if end > start
    ]


def backtrack_breath_mask_starts(
    mask: np.ndarray,
    rms_energy: np.ndarray,
    max_backtrack_frames: int,
    silence_ratio: float = 0.18,
    onset_ratio: float = 0.28,
) -> np.ndarray:
    if mask.size == 0 or rms_energy.size == 0:
        return mask

    frame_count = min(mask.size, rms_energy.size)
    result = mask.copy()
    energy = np.maximum(rms_energy[:frame_count].astype(np.float32), 0.0)
    active_energy = energy[mask[:frame_count]]
    if active_energy.size == 0:
        return result

    reference = float(np.quantile(active_energy, 0.70))
    if reference <= 1.0e-8:
        return result

    silence_threshold = reference * silence_ratio
    onset_threshold = reference * onset_ratio

    padded = np.pad(mask[:frame_count].astype(np.int8), (1, 1))
    edges = np.diff(padded)
    starts = np.flatnonzero(edges == 1)
    for start in starts:
        search_start = max(0, int(start) - max_backtrack_frames)
        if search_start >= start:
            continue

        window = energy[search_start : int(start) + 1]
        crossings = np.flatnonzero(window >= onset_threshold)
        if crossings.size == 0:
            continue

        candidate = search_start + int(crossings[0])
        quiet = np.flatnonzero(energy[search_start:candidate] <= silence_threshold)
        if quiet.size:
            candidate = search_start + int(quiet[-1]) + 1

        if candidate < start:
            result[candidate:start] = True
    return result


def normalize_novelty(values: np.ndarray) -> np.ndarray:
    values = np.maximum(values.astype(np.float32), 0.0)
    scale = float(np.quantile(values, 0.95))
    if scale <= 1.0e-8:
        scale = float(np.max(values))
    if scale <= 1.0e-8:
        return np.zeros_like(values)
    return np.clip(values / scale, 0.0, 1.0)


def refine_onset_frames(
    predicted_frames: np.ndarray,
    onset_novelty: np.ndarray,
    rms_novelty: np.ndarray,
    rms_energy: np.ndarray,
    min_distance_frames: int,
    search_before_frames: int = 18,
    search_after_frames: int = 5,
    backtrack_frames: int = 14,
) -> np.ndarray:
    if predicted_frames.size == 0:
        return predicted_frames.astype(np.int64)

    frame_count = min(len(onset_novelty), len(rms_novelty), len(rms_energy))
    onset = normalize_novelty(onset_novelty[:frame_count])
    rms = normalize_novelty(rms_novelty[:frame_count])
    energy = normalize_novelty(rms_energy[:frame_count])
    combined = 0.72 * onset + 0.28 * rms

    refined: list[int] = []
    for predicted in predicted_frames:
        start = max(0, int(predicted) - search_before_frames)
        end = min(frame_count, int(predicted) + search_after_frames + 1)
        if end <= start:
            candidate = int(predicted)
        else:
            frames = np.arange(start, end)
            local = combined[start:end]
            peaks = frames[
                (local >= np.pad(local[:-1], (1, 0), constant_values=-np.inf))
                & (local > np.pad(local[1:], (0, 1), constant_values=-np.inf))
            ]
            if peaks.size == 0:
                peaks = np.asarray([frames[int(np.argmax(local))]])

            best_score = -np.inf
            candidate = int(predicted)
            for peak in peaks:
                valley_start = max(0, int(peak) - backtrack_frames)
                valley = valley_start + int(
                    np.argmin(energy[valley_start : int(peak) + 1])
                )
                peak_energy = float(
                    np.max(energy[int(peak) : min(frame_count, int(peak) + 4)])
                )
                valley_energy = float(energy[valley])
                rise = max(0.0, peak_energy - valley_energy)

                attack_start = valley
                if rise > 0.05:
                    threshold = valley_energy + rise * 0.18
                    crossing = np.flatnonzero(
                        energy[valley : int(peak) + 1] >= threshold
                    )
                    if crossing.size:
                        attack_start = valley + int(crossing[0])

                distance_penalty = 0.018 * abs(attack_start - int(predicted))
                score = float(combined[int(peak)]) + 0.55 * rise - distance_penalty
                if score > best_score:
                    best_score = score
                    candidate = attack_start

            # Do not move a confident TCN onset when the local acoustic cue is weak.
            if best_score < 0.12:
                candidate = int(predicted)

        if refined and candidate - refined[-1] < min_distance_frames:
            candidate = max(int(predicted), refined[-1] + min_distance_frames)
        refined.append(min(candidate, max(0, frame_count - 1)))
    return np.asarray(refined, dtype=np.int64)


def acoustic_novelty(
    audio_path: Path,
    sample_rate: int,
    hop_length: int,
    n_fft: int,
    frame_count: int,
) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    audio, _ = librosa.load(audio_path, sr=sample_rate, mono=True)
    onset = librosa.onset.onset_strength(
        y=audio,
        sr=sample_rate,
        hop_length=hop_length,
        n_fft=n_fft,
    )
    rms = librosa.feature.rms(
        y=audio,
        frame_length=n_fft,
        hop_length=hop_length,
    )[0]
    rms = np.convolve(rms, np.ones(5, dtype=np.float32) / 5.0, mode="same")
    rms_delta = np.maximum(np.diff(rms, prepend=rms[0]), 0.0)

    def fit(values: np.ndarray) -> np.ndarray:
        if len(values) >= frame_count:
            return values[:frame_count]
        return np.pad(values, (0, frame_count - len(values)))

    return fit(onset), fit(rms_delta), fit(rms)


def main() -> int:
    args = parse_args()
    project_root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(project_root))

    from research.gtsinger_alignment import train_baseline as model_api

    checkpoint = torch.load(
        args.checkpoint.expanduser().resolve(),
        map_location="cpu",
        weights_only=True,
    )
    model_metadata = checkpoint.get("model", {})
    if model_metadata.get("type") != "BoundaryNetResidualTCN":
        raise RuntimeError("checkpoint does not contain the supported residual TCN")
    expected = {
        "sample_rate": model_api.TARGET_SAMPLE_RATE,
        "hop_length": model_api.HOP_LENGTH,
        "n_mels": model_api.N_MELS,
    }
    for name, value in expected.items():
        if model_metadata.get(name) != value:
            raise RuntimeError(
                f"checkpoint {name}={model_metadata.get(name)!r}, expected {value!r}"
            )
    model = model_api.BoundaryNet(model_api.N_MELS)
    model.load_state_dict(checkpoint["model_state"])
    device = torch.device(
        "mps"
        if torch.backends.mps.is_available()
        else "cuda"
        if torch.cuda.is_available()
        else "cpu"
    )
    model.to(device).eval()

    features = model_api.make_features(args.audio.expanduser().resolve())
    with torch.inference_mode():
        tensor = torch.from_numpy(features).unsqueeze(0).to(device)
        probabilities = torch.sigmoid(model(tensor)).squeeze(0).cpu().numpy()
    onset_novelty, rms_novelty, rms_energy = acoustic_novelty(
        args.audio.expanduser().resolve(),
        model_api.TARGET_SAMPLE_RATE,
        model_api.HOP_LENGTH,
        model_api.N_FFT,
        probabilities.shape[1],
    )

    training = checkpoint["training"]
    thresholds = training["calibrated_thresholds"]
    frame_seconds = model_api.HOP_LENGTH / model_api.TARGET_SAMPLE_RATE
    min_distance_frames = max(
        1,
        round(
            training["event_min_distance_ms"]
            * model_api.TARGET_SAMPLE_RATE
            / (1000.0 * model_api.HOP_LENGTH)
        ),
    )

    events: dict[str, list[dict[str, float]]] = {}
    intervals: dict[str, list[dict[str, float]]] = {}
    for index, name in enumerate(model_api.EVENT_NAMES):
        threshold = float(thresholds[name])
        if name in model_api.ONSET_HEADS:
            frames = model_api.extract_event_frames(
                probabilities[index],
                threshold,
                min_distance_frames,
            )
            raw_frames = frames
            if name == "syllable":
                frames = refine_onset_frames(
                    frames,
                    onset_novelty,
                    rms_novelty,
                    rms_energy,
                    min_distance_frames,
                )
            events[name] = [
                {
                    "time": round(float(frame * frame_seconds), 6),
                    "raw_time": round(float(raw_frame * frame_seconds), 6),
                    "confidence": round(
                        float(probabilities[index, min(raw_frame, probabilities.shape[1] - 1)]),
                        6,
                    ),
                }
                for frame, raw_frame in zip(frames, raw_frames, strict=True)
            ]
    interval_indices = [
        model_api.EVENT_NAMES.index(name) for name in model_api.INTERVAL_HEADS
    ]
    interval_probabilities = probabilities[interval_indices]
    interval_thresholds = tuple(
        float(thresholds[name]) for name in model_api.INTERVAL_HEADS
    )
    interval_masks = clean_interval_masks_with_breath_hysteresis(
        interval_probabilities,
        interval_thresholds,
        min_frames=max(1, round(0.08 / frame_seconds)),
        merge_gap_frames=max(1, round(0.05 / frame_seconds)),
    )
    for index, name in enumerate(model_api.INTERVAL_HEADS):
        intervals[name] = mask_to_intervals(
            interval_masks[index],
            interval_probabilities[index],
            frame_seconds,
        )

    print(
        json.dumps(
            {
                "model": checkpoint["model"]["type"],
                "device": str(device),
                "frame_seconds": frame_seconds,
                "onset_refinement": {
                    "syllable": "spectral_flux_plus_rms_rise",
                    "search_before_ms": 180,
                    "search_after_ms": 50,
                    "backtrack_to_rms_rise_ms": 140,
                },
                "interval_decoding": {
                    "breath": "hysteresis_peak_seeded_mask",
                    "silence": "fixed_threshold_mask",
                },
                "thresholds": thresholds,
                "events": events,
                "intervals": intervals,
            }
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
