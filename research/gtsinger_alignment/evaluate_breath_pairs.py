#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

try:
    import build_breath_pair_manifest as breath_pairs
except ImportError:
    from . import build_breath_pair_manifest as breath_pairs


def interval_iou(left: list[float], right: list[float]) -> float:
    intersection = max(0.0, min(left[1], right[1]) - max(left[0], right[0]))
    union = max(left[1], right[1]) - min(left[0], right[0])
    return intersection / union if union > 0.0 else 0.0


def centers_match(prediction: list[float], target: list[float], tolerance: float) -> bool:
    center = (prediction[0] + prediction[1]) * 0.5
    return target[0] - tolerance <= center <= target[1] + tolerance


def match_intervals(
    predictions: list[list[float]],
    targets: list[list[float]],
    iou_threshold: float,
    center_tolerance: float,
) -> tuple[list[tuple[int, int]], list[int], list[int]]:
    candidates = []
    for prediction_index, prediction in enumerate(predictions):
        for target_index, target in enumerate(targets):
            iou = interval_iou(prediction, target)
            if iou >= iou_threshold or centers_match(prediction, target, center_tolerance):
                candidates.append((iou, prediction_index, target_index))

    matches = []
    used_predictions = set()
    used_targets = set()
    for _, prediction_index, target_index in sorted(candidates, reverse=True):
        if prediction_index in used_predictions or target_index in used_targets:
            continue
        used_predictions.add(prediction_index)
        used_targets.add(target_index)
        matches.append((prediction_index, target_index))

    false_positives = [
        index for index in range(len(predictions)) if index not in used_predictions
    ]
    false_negatives = [
        index for index in range(len(targets)) if index not in used_targets
    ]
    return matches, false_positives, false_negatives


def target_breath_intervals(breath_path: Path) -> list[list[float]]:
    audio, sample_rate = breath_pairs.load_mono(breath_path)
    mask, frames_per_second = breath_pairs.breath_rms_mask(audio, sample_rate)
    mask = breath_pairs.close_mask(mask, max_gap_frames=20)
    return breath_pairs.mask_to_intervals(
        mask,
        frames_per_second,
        min_duration_seconds=0.18,
    )


def predicted_breath_intervals(
    python_executable: Path,
    inference_script: Path,
    checkpoint: Path,
    vocal_path: Path,
) -> list[list[float]]:
    output = subprocess.check_output(
        [
            str(python_executable),
            str(inference_script),
            str(vocal_path),
            "--checkpoint",
            str(checkpoint),
        ],
        text=True,
    )
    decoded = json.loads(output)
    return [
        [float(interval["start"]), float(interval["end"])]
        for interval in decoded["intervals"]["breath"]
    ]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Evaluate breath interval detection on vocal/breath pairs."
    )
    parser.add_argument("--dataset-root", type=Path, required=True)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--split", default="test")
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--python", type=Path, required=True)
    parser.add_argument("--inference-script", type=Path, required=True)
    parser.add_argument("--iou-threshold", type=float, default=0.25)
    parser.add_argument("--center-tolerance", type=float, default=0.10)
    return parser.parse_args()


def pairs_from_manifest(
    dataset_root: Path,
    manifest_path: Path,
    split: str,
) -> list[tuple[Path, Path]]:
    pairs = []
    with manifest_path.open("r", encoding="utf-8") as manifest_file:
        for line_number, line in enumerate(manifest_file, start=1):
            if not line.strip():
                continue
            record = json.loads(line)
            if record.get("split") != split:
                continue
            breath = record.get("paired_breath")
            if not breath:
                raise ValueError(f"line {line_number}: missing paired_breath")
            pairs.append((dataset_root / record["audio"], dataset_root / breath))
    return pairs


def main() -> int:
    args = parse_args()
    dataset_root = args.dataset_root.expanduser().resolve()
    checkpoint = args.checkpoint.expanduser().resolve()
    python_executable = args.python.expanduser().resolve()
    inference_script = args.inference_script.expanduser().resolve()

    pairs = (
        pairs_from_manifest(dataset_root, args.manifest.expanduser().resolve(), args.split)
        if args.manifest is not None
        else breath_pairs.pair_files(dataset_root)
    )
    per_file = []
    total_true_positive = 0
    total_false_positive = 0
    total_false_negative = 0

    for vocal_path, breath_path in pairs:
        targets = target_breath_intervals(breath_path)
        predictions = predicted_breath_intervals(
            python_executable,
            inference_script,
            checkpoint,
            vocal_path,
        )
        matches, false_positives, false_negatives = match_intervals(
            predictions,
            targets,
            args.iou_threshold,
            args.center_tolerance,
        )
        total_true_positive += len(matches)
        total_false_positive += len(false_positives)
        total_false_negative += len(false_negatives)
        per_file.append(
            {
                "vocal": vocal_path.name,
                "breath": breath_path.name,
                "targets": len(targets),
                "predictions": len(predictions),
                "true_positive": len(matches),
                "false_positive": len(false_positives),
                "false_negative": len(false_negatives),
                "missed": [targets[index] for index in false_negatives],
                "extra": [predictions[index] for index in false_positives],
            }
        )

    precision = total_true_positive / max(1, total_true_positive + total_false_positive)
    recall = total_true_positive / max(1, total_true_positive + total_false_negative)
    f1 = 2.0 * precision * recall / max(1.0e-12, precision + recall)
    print(
        json.dumps(
            {
                "dataset_root": str(dataset_root),
                "pairs": len(pairs),
                "manifest": str(args.manifest.expanduser().resolve()) if args.manifest else None,
                "split": args.split if args.manifest else None,
                "checkpoint": str(checkpoint),
                "matching": {
                    "iou_threshold": args.iou_threshold,
                    "center_tolerance": args.center_tolerance,
                },
                "metrics": {
                    "precision": round(precision, 4),
                    "recall": round(recall, 4),
                    "f1": round(f1, 4),
                    "true_positive": total_true_positive,
                    "false_positive": total_false_positive,
                    "false_negative": total_false_negative,
                },
                "files": per_file,
            },
            indent=2,
        )
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
