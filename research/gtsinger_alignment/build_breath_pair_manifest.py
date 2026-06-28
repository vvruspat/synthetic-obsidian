#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf


MANIFEST_VERSION = 1
TARGET_SAMPLE_RATE = 16_000
HOP_LENGTH = 160
FRAME_LENGTH = 512


def load_mono(path: Path) -> tuple[np.ndarray, int]:
    audio, sample_rate = sf.read(path, dtype="float32", always_2d=True)
    mono = np.mean(audio, axis=1)
    return mono, sample_rate


def rms_values(audio: np.ndarray, sample_rate: int) -> tuple[np.ndarray, float]:
    if sample_rate != TARGET_SAMPLE_RATE:
        audio = librosa.resample(
            audio,
            orig_sr=sample_rate,
            target_sr=TARGET_SAMPLE_RATE,
        )
    rms = librosa.feature.rms(
        y=audio,
        frame_length=FRAME_LENGTH,
        hop_length=HOP_LENGTH,
    )[0]
    return np.convolve(rms, np.ones(5, dtype=np.float32) / 5.0, mode="same"), TARGET_SAMPLE_RATE / HOP_LENGTH


def rms_mask(audio: np.ndarray, sample_rate: int) -> tuple[np.ndarray, float]:
    smoothed, frames_per_second = rms_values(audio, sample_rate)
    floor = float(np.quantile(smoothed, 0.50))
    strong = float(np.quantile(smoothed, 0.95))
    peakish = float(np.quantile(smoothed, 0.98))
    # Breath stems often contain very quiet residual noise. Use a conservative
    # gate so the fine-tune learns obvious breaths, not every low-level tick.
    threshold = max(floor * 8.0, strong * 0.55, peakish * 0.35, 5.0e-4)
    return smoothed >= threshold, frames_per_second


def strong_vocal_mask(audio: np.ndarray, sample_rate: int) -> tuple[np.ndarray, float]:
    smoothed, frames_per_second = rms_values(audio, sample_rate)
    floor = float(np.quantile(smoothed, 0.50))
    strong = float(np.quantile(smoothed, 0.95))
    peakish = float(np.quantile(smoothed, 0.98))
    threshold = max(floor * 12.0, strong * 0.62, peakish * 0.45, 1.0e-3)
    return smoothed >= threshold, frames_per_second


def breath_rms_mask(audio: np.ndarray, sample_rate: int) -> tuple[np.ndarray, float]:
    if sample_rate != TARGET_SAMPLE_RATE:
        audio = librosa.resample(
            audio,
            orig_sr=sample_rate,
            target_sr=TARGET_SAMPLE_RATE,
        )

    frame_count = max(1, 1 + max(0, len(audio) - FRAME_LENGTH) // HOP_LENGTH)
    activity = np.zeros(frame_count, dtype=bool)
    # These stems are curated masks rendered as audio: before a breath they are
    # digitally silent. Do not RMS-gate them; any non-zero sample in the breath
    # stem means the frame belongs to the breath target.
    threshold = 1.0e-8
    for frame in range(frame_count):
        start = frame * HOP_LENGTH
        end = min(len(audio), start + FRAME_LENGTH)
        if end > start and float(np.max(np.abs(audio[start:end]))) > threshold:
            activity[frame] = True
    return activity, TARGET_SAMPLE_RATE / HOP_LENGTH


def trim_breath_mask_by_vocal_difference(
    mask: np.ndarray,
    vocal_audio: np.ndarray,
    vocal_sample_rate: int,
    breath_audio: np.ndarray,
    breath_sample_rate: int,
    minimum_keep_frames: int,
) -> np.ndarray:
    result = mask.copy()
    if vocal_sample_rate != TARGET_SAMPLE_RATE:
        vocal_audio = librosa.resample(
            vocal_audio,
            orig_sr=vocal_sample_rate,
            target_sr=TARGET_SAMPLE_RATE,
        )
    if breath_sample_rate != TARGET_SAMPLE_RATE:
        breath_audio = librosa.resample(
            breath_audio,
            orig_sr=breath_sample_rate,
            target_sr=TARGET_SAMPLE_RATE,
        )

    sample_count = min(len(vocal_audio), len(breath_audio))
    if sample_count <= 0:
        return result
    vocal_audio = vocal_audio[:sample_count]
    breath_audio = breath_audio[:sample_count]
    residual_audio = vocal_audio - breath_audio
    breath_rms, _ = rms_values(breath_audio, TARGET_SAMPLE_RATE)
    residual_rms, _ = rms_values(residual_audio, TARGET_SAMPLE_RATE)
    frame_count = min(len(result), len(breath_rms), len(residual_rms))
    if frame_count <= 0:
        return result

    ratio = residual_rms[:frame_count] / np.maximum(breath_rms[:frame_count], 1.0e-7)
    padded = np.pad(result.astype(np.int8), (1, 1))
    edges = np.diff(padded)
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    for start, end in zip(starts, ends, strict=True):
        end = min(end, frame_count)
        if end - start <= minimum_keep_frames:
            continue

        reference_end = min(end, start + max(minimum_keep_frames, (end - start) // 2))
        local_reference = ratio[start:reference_end]
        finite_reference = local_reference[np.isfinite(local_reference)]
        if finite_reference.size == 0:
            continue

        baseline = float(np.quantile(finite_reference, 0.35))
        mismatch_threshold = max(1.15, min(24.0, baseline * 6.0))
        search_start = start + minimum_keep_frames
        mismatches = ratio[search_start:end] >= mismatch_threshold
        if mismatches.size < 2:
            continue

        sustained = mismatches[:-1] & mismatches[1:]
        if not np.any(sustained):
            continue

        cut = search_start + int(np.flatnonzero(sustained)[0])
        if cut < end:
            result[cut:end] = False
    return result


def close_mask(mask: np.ndarray, max_gap_frames: int) -> np.ndarray:
    result = mask.copy()
    padded = np.pad(result.astype(np.int8), (1, 1))
    edges = np.diff(padded)
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    for previous_end, next_start in zip(ends[:-1], starts[1:], strict=True):
        if next_start - previous_end <= max_gap_frames:
            result[previous_end:next_start] = True
    return result


def mask_to_intervals(
    mask: np.ndarray,
    frames_per_second: float,
    min_duration_seconds: float,
) -> list[list[float]]:
    padded = np.pad(mask.astype(np.int8), (1, 1))
    edges = np.diff(padded)
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    intervals = []
    for start, end in zip(starts, ends, strict=True):
        start_seconds = start / frames_per_second
        end_seconds = end / frames_per_second
        if end_seconds - start_seconds >= min_duration_seconds:
            intervals.append([round(start_seconds, 6), round(end_seconds, 6)])
    return intervals


def interval_starts(intervals: list[list[float]]) -> list[float]:
    return [round(float(start), 6) for start, _ in intervals]


def pair_key(path: Path, prefix: str) -> str:
    stem = path.stem.lower()
    if stem == prefix:
        return ""
    if not stem.startswith(prefix):
        raise ValueError(f"{path.name!r} does not start with {prefix!r}")
    suffix = stem[len(prefix) :]
    return suffix.lstrip("_- ")


def natural_key(value: str) -> tuple[int, str]:
    if value == "":
        return (0, "")
    match = re.fullmatch(r"\d+", value)
    if match:
        return (int(value), "")
    return (10**9, value)


def pair_files(dataset_root: Path) -> list[tuple[Path, Path]]:
    files = sorted(
        path
        for path in dataset_root.iterdir()
        if path.is_file() and path.suffix.lower() in {".wav", ".aif", ".aiff", ".flac"}
    )
    vocals: dict[str, Path] = {}
    breaths: dict[str, Path] = {}
    for path in files:
        stem = path.stem.lower()
        if stem.startswith("vox"):
            vocals[pair_key(path, "vox")] = path
        elif stem.startswith("voice"):
            vocals[pair_key(path, "voice")] = path
        elif stem.startswith("breath"):
            breaths[pair_key(path, "breath")] = path

    missing_breaths = sorted(set(vocals) - set(breaths), key=natural_key)
    missing_vocals = sorted(set(breaths) - set(vocals), key=natural_key)
    if missing_breaths or missing_vocals:
        raise ValueError(
            "unpaired files: "
            f"missing breaths for {missing_breaths[:8]}, "
            f"missing vocals for {missing_vocals[:8]}"
        )

    return [
        (vocals[key], breaths[key])
        for key in sorted(vocals, key=natural_key)
    ]


def split_for_pair(key: str, validation_ratio: float, test_ratio: float) -> str:
    bucket = int(hashlib.sha1(key.encode("utf-8")).hexdigest()[:8], 16) / 0xFFFFFFFF
    if bucket < test_ratio:
        return "test"
    if bucket < test_ratio + validation_ratio:
        return "validation"
    return "train"


def build_record(
    dataset_root: Path,
    vocal_path: Path,
    breath_path: Path,
    split: str,
) -> dict[str, object]:
    vocal_audio, vocal_sample_rate = load_mono(vocal_path)
    breath_audio, breath_sample_rate = load_mono(breath_path)
    duration_seconds = min(
        len(vocal_audio) / vocal_sample_rate,
        len(breath_audio) / breath_sample_rate,
    )

    breath_mask, frames_per_second = breath_rms_mask(breath_audio, breath_sample_rate)
    vocal_mask, _ = rms_mask(vocal_audio, vocal_sample_rate)
    frame_count = min(len(breath_mask), len(vocal_mask))
    breath_mask = close_mask(breath_mask[:frame_count], max_gap_frames=20)
    breath_mask = trim_breath_mask_by_vocal_difference(
        breath_mask,
        vocal_audio,
        vocal_sample_rate,
        breath_audio,
        breath_sample_rate,
        minimum_keep_frames=round(0.12 * frames_per_second),
    )
    vocal_mask = close_mask(vocal_mask[:frame_count], max_gap_frames=8)

    breath_intervals = mask_to_intervals(
        breath_mask,
        frames_per_second,
        min_duration_seconds=0.18,
    )
    silence_intervals = mask_to_intervals(
        ~(breath_mask | vocal_mask),
        frames_per_second,
        min_duration_seconds=0.12,
    )
    relative_audio = vocal_path.relative_to(dataset_root).as_posix()
    relative_breath = breath_path.relative_to(dataset_root).as_posix()
    record_id = hashlib.sha1(
        f"{relative_audio}:{breath_path.name}".encode("utf-8")
    ).hexdigest()[:16]
    return {
        "manifest_version": MANIFEST_VERSION,
        "id": record_id,
        "audio": relative_audio,
        "paired_breath": relative_breath,
        "annotation": None,
        "split": split,
        "group_key": "breath_pair_dataset",
        "language": "unknown",
        "singer": "user",
        "technique": "breath_pairs",
        "song": "breath_pair_dataset",
        "group": "breath_pair_dataset",
        "clip": vocal_path.stem,
        "sample_rate": vocal_sample_rate,
        "channels": 1,
        "num_frames": int(round(duration_seconds * vocal_sample_rate)),
        "duration_seconds": round(duration_seconds, 6),
        "events": {
            "phoneme": [],
            "syllable": [],
            "breath": interval_starts(breath_intervals),
            "silence": interval_starts(silence_intervals),
        },
        "intervals": {
            "breath": breath_intervals,
            "silence": silence_intervals,
        },
        "event_counts": {
            "phoneme": 0,
            "syllable": 0,
            "breath": len(breath_intervals),
            "silence": len(silence_intervals),
        },
        "loss_heads": ["breath", "silence"],
        "label_notes": {
            "breath": f"RMS activity from paired breath stem {breath_path.name}",
            "silence": "RMS inactivity where neither vocal nor breath stem is active",
            "loss_heads": "only breath/silence heads are trained from this dataset",
        },
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a manifest from sorted vocal/breath audio pairs."
    )
    parser.add_argument("--dataset-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--validation-ratio", type=float, default=0.0)
    parser.add_argument("--test-ratio", type=float, default=0.0)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    dataset_root = args.dataset_root.expanduser().resolve()
    if args.validation_ratio < 0.0 or args.test_ratio < 0.0:
        raise ValueError("split ratios must be non-negative")
    if args.validation_ratio + args.test_ratio >= 1.0:
        raise ValueError("validation-ratio + test-ratio must be less than 1")

    pairs = pair_files(dataset_root)
    records = [
        build_record(
            dataset_root,
            vocal,
            breath,
            split_for_pair(
                vocal.stem,
                args.validation_ratio,
                args.test_ratio,
            ),
        )
        for vocal, breath in pairs
    ]
    summary = {
        "dataset_root": str(dataset_root),
        "pairs": len(pairs),
        "splits": {
            split: sum(1 for record in records if record["split"] == split)
            for split in ("train", "validation", "test")
        },
        "breath_intervals": sum(record["event_counts"]["breath"] for record in records),
        "silence_intervals": sum(record["event_counts"]["silence"] for record in records),
    }
    if args.dry_run:
        print(json.dumps(summary, indent=2))
        return 0

    output = args.output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    with output.open("w", encoding="utf-8") as manifest_file:
        for record in records:
            manifest_file.write(json.dumps(record, ensure_ascii=False) + "\n")
    print(json.dumps({**summary, "output": str(output)}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        raise SystemExit(1)
