#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import random
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import librosa
import numpy as np
import soundfile as sf
import torch
from torch import nn
from torch.utils.data import DataLoader, Dataset

try:
    from .labels import derive_labels
except ImportError:
    from labels import derive_labels


EVENT_NAMES = ("phoneme", "syllable", "breath", "silence")
ONSET_HEADS = ("phoneme", "syllable")
INTERVAL_HEADS = ("breath", "silence")
HEAD_TASKS = {
    "phoneme": "onset",
    "syllable": "onset",
    "breath": "framewise_segmentation",
    "silence": "framewise_segmentation",
}
TARGET_SAMPLE_RATE = 16_000
HOP_LENGTH = 160
N_FFT = 512
N_MELS = 64
TCN_CHANNELS = 64
TCN_KERNEL_SIZE = 3
TCN_DILATIONS = (1, 2, 4, 8)


@dataclass(frozen=True)
class Example:
    features: np.ndarray
    targets: np.ndarray
    event_frames: tuple[np.ndarray, ...]
    record_id: str


class ChunkDataset(Dataset[tuple[torch.Tensor, torch.Tensor]]):
    def __init__(self, examples: list[Example], chunk_frames: int):
        self.examples = examples
        self.chunk_frames = chunk_frames
        self.index: list[tuple[int, int]] = []
        stride = max(1, chunk_frames // 2)
        for example_index, example in enumerate(examples):
            frame_count = example.features.shape[1]
            if frame_count <= chunk_frames:
                self.index.append((example_index, 0))
                continue
            starts = list(range(0, frame_count - chunk_frames + 1, stride))
            final_start = frame_count - chunk_frames
            if starts[-1] != final_start:
                starts.append(final_start)
            self.index.extend((example_index, start) for start in starts)

    def __len__(self) -> int:
        return len(self.index)

    def __getitem__(self, index: int) -> tuple[torch.Tensor, torch.Tensor]:
        example_index, start = self.index[index]
        example = self.examples[example_index]
        end = start + self.chunk_frames
        features = example.features[:, start:end]
        targets = example.targets[:, start:end]
        if features.shape[1] < self.chunk_frames:
            pad = self.chunk_frames - features.shape[1]
            features = np.pad(features, ((0, 0), (0, pad)))
            targets = np.pad(targets, ((0, 0), (0, pad)))
        return torch.from_numpy(features), torch.from_numpy(targets)


def receptive_field_frames(
    kernel_size: int = TCN_KERNEL_SIZE,
    dilations: tuple[int, ...] = TCN_DILATIONS,
) -> int:
    return 1 + (kernel_size - 1) * sum(dilations)


class ResidualDilatedBlock(nn.Module):
    def __init__(self, channels: int, kernel_size: int, dilation: int):
        super().__init__()
        padding = dilation * (kernel_size - 1) // 2
        self.temporal = nn.Conv1d(
            channels,
            channels,
            kernel_size=kernel_size,
            padding=padding,
            dilation=dilation,
        )
        self.mix = nn.Conv1d(channels, channels, kernel_size=1)
        self.activation = nn.ReLU()

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        residual = features
        features = self.activation(self.temporal(features))
        return self.activation(self.mix(features) + residual)


class BoundaryNet(nn.Module):
    def __init__(self, feature_count: int):
        super().__init__()
        self.input_projection = nn.Sequential(
            nn.Conv1d(feature_count, TCN_CHANNELS, kernel_size=1),
            nn.ReLU(),
        )
        self.temporal_blocks = nn.Sequential(
            *(
                ResidualDilatedBlock(
                    TCN_CHANNELS,
                    kernel_size=TCN_KERNEL_SIZE,
                    dilation=dilation,
                )
                for dilation in TCN_DILATIONS
            )
        )
        self.output_heads = nn.Conv1d(
            TCN_CHANNELS, len(EVENT_NAMES), kernel_size=1
        )

    def forward(self, features: torch.Tensor) -> torch.Tensor:
        features = self.input_projection(features)
        features = self.temporal_blocks(features)
        return self.output_heads(features)


def seed_everything(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)


def load_manifest(path: Path) -> list[dict[str, Any]]:
    records = []
    with path.open("r", encoding="utf-8") as manifest_file:
        for line_number, line in enumerate(manifest_file, start=1):
            if not line.strip():
                continue
            record = json.loads(line)
            if record.get("manifest_version") != 1:
                raise ValueError(f"line {line_number}: unsupported manifest version")
            if not all(name in record.get("events", {}) for name in EVENT_NAMES):
                raise ValueError(f"line {line_number}: missing event labels")
            if not all(name in record.get("intervals", {}) for name in INTERVAL_HEADS):
                raise ValueError(f"line {line_number}: missing interval labels")
            records.append(record)
    return records


def select_records(
    records: list[dict[str, Any]], split: str, max_records: int | None, seed: int
) -> list[dict[str, Any]]:
    selected = [record for record in records if record["split"] == split]
    selected.sort(
        key=lambda record: (
            record["group_key"],
            record["id"],
        )
    )
    random.Random(seed).shuffle(selected)
    return selected if max_records is None else selected[:max_records]


def make_features(audio_path: Path) -> np.ndarray:
    audio, sample_rate = sf.read(audio_path, dtype="float32", always_2d=True)
    mono = np.mean(audio, axis=1)
    if sample_rate != TARGET_SAMPLE_RATE:
        mono = librosa.resample(
            mono, orig_sr=sample_rate, target_sr=TARGET_SAMPLE_RATE
        )
    mel = librosa.feature.melspectrogram(
        y=mono,
        sr=TARGET_SAMPLE_RATE,
        n_fft=N_FFT,
        hop_length=HOP_LENGTH,
        n_mels=N_MELS,
        fmin=40.0,
        fmax=7_600.0,
        power=2.0,
    )
    log_mel = librosa.power_to_db(mel, ref=np.max)
    mean = np.mean(log_mel, axis=1, keepdims=True)
    std = np.std(log_mel, axis=1, keepdims=True)
    return ((log_mel - mean) / np.maximum(std, 1.0e-5)).astype(np.float32)


def make_targets(
    events: dict[str, list[float]],
    intervals: dict[str, list[list[float]]],
    frame_count: int,
    tolerance_ms: float,
) -> np.ndarray:
    targets = np.zeros((len(EVENT_NAMES), frame_count), dtype=np.float32)
    radius = max(
        0,
        int(round((tolerance_ms / 1000.0) * TARGET_SAMPLE_RATE / HOP_LENGTH)),
    )
    for event_index, event_name in enumerate(ONSET_HEADS):
        for time_seconds in events[event_name]:
            frame = int(round(float(time_seconds) * TARGET_SAMPLE_RATE / HOP_LENGTH))
            start = max(0, frame - radius)
            end = min(frame_count, frame + radius + 1)
            targets[event_index, start:end] = 1.0

    for interval_name in INTERVAL_HEADS:
        head_index = EVENT_NAMES.index(interval_name)
        for start_seconds, end_seconds in intervals[interval_name]:
            start = max(
                0,
                int(np.floor(float(start_seconds) * TARGET_SAMPLE_RATE / HOP_LENGTH)),
            )
            end = min(
                frame_count,
                int(np.ceil(float(end_seconds) * TARGET_SAMPLE_RATE / HOP_LENGTH)),
            )
            if end > start:
                targets[head_index, start:end] = 1.0
    return targets


def events_to_frames(events: dict[str, list[float]]) -> tuple[np.ndarray, ...]:
    return tuple(
        np.asarray(
            [
                int(round(float(time_seconds) * TARGET_SAMPLE_RATE / HOP_LENGTH))
                for time_seconds in events[event_name]
            ],
            dtype=np.int64,
        )
        for event_name in EVENT_NAMES
    )


def load_examples(
    records: list[dict[str, Any]],
    dataset_root: Path,
    tolerance_ms: float,
    rederive_syllable_labels: bool,
) -> list[Example]:
    examples = []
    for index, record in enumerate(records, start=1):
        audio_path = dataset_root / record["audio"]
        features = make_features(audio_path)
        events = record["events"]
        if rederive_syllable_labels:
            annotation_path = dataset_root / record["annotation"]
            with annotation_path.open("r", encoding="utf-8") as annotation_file:
                annotation = json.load(annotation_file)
            refreshed = derive_labels(
                annotation,
                float(record["duration_seconds"]),
                language=str(record.get("language", "")) or None,
            )
            events = {**events, "syllable": refreshed.events["syllable"]}
        targets = make_targets(
            events,
            record["intervals"],
            features.shape[1],
            tolerance_ms,
        )
        loss_heads = record.get("loss_heads")
        if loss_heads is not None:
            enabled = set(str(name) for name in loss_heads)
            for head_index, event_name in enumerate(EVENT_NAMES):
                if event_name not in enabled:
                    targets[head_index, :] = np.nan
        event_frames = events_to_frames(events)
        examples.append(Example(features, targets, event_frames, str(record["id"])))
        print(
            f"loaded {index}/{len(records)} {record['id']} "
            f"frames={features.shape[1]}",
            file=sys.stderr,
        )
    return examples


def restrict_examples_to_heads(examples: list[Example], train_heads: list[str] | None) -> list[Example]:
    if train_heads is None:
        return examples

    enabled = set(train_heads)
    restricted = []
    for example in examples:
        targets = example.targets.copy()
        for head_index, event_name in enumerate(EVENT_NAMES):
            if event_name not in enabled:
                targets[head_index, :] = np.nan
        restricted.append(Example(example.features, targets, example.event_frames, example.record_id))
    return restricted


def positive_weights(examples: list[Example]) -> torch.Tensor:
    positives = np.zeros(len(EVENT_NAMES), dtype=np.float64)
    totals = np.zeros(len(EVENT_NAMES), dtype=np.float64)
    for example in examples:
        active = np.isfinite(example.targets)
        positives += np.nansum(example.targets, axis=1)
        totals += np.sum(active, axis=1)
    negatives = np.maximum(1.0, totals - positives)
    weights = negatives / np.maximum(1.0, positives)
    for event_name in INTERVAL_HEADS:
        event_index = EVENT_NAMES.index(event_name)
        weights[event_index] = np.sqrt(weights[event_index])
    weights = np.clip(weights, 1.0, 100.0)
    return torch.tensor(weights, dtype=torch.float32)


def training_loss(
    logits: torch.Tensor,
    targets: torch.Tensor,
    positive_weight: torch.Tensor,
    syllable_hard_negative_weight: float = 1.0,
) -> torch.Tensor:
    active = torch.isfinite(targets)
    clean_targets = torch.where(active, targets, torch.zeros_like(targets))
    losses = nn.functional.binary_cross_entropy_with_logits(
        logits,
        clean_targets,
        pos_weight=positive_weight.view(1, -1, 1),
        reduction="none",
    )
    if syllable_hard_negative_weight > 1.0:
        phoneme_index = EVENT_NAMES.index("phoneme")
        syllable_index = EVENT_NAMES.index("syllable")
        hard_negatives = (clean_targets[:, phoneme_index] > 0.5) & (
            clean_targets[:, syllable_index] < 0.5
        )
        losses[:, syllable_index] *= torch.where(
            hard_negatives,
            torch.as_tensor(
                syllable_hard_negative_weight,
                dtype=losses.dtype,
                device=losses.device,
            ),
            torch.ones((), dtype=losses.dtype, device=losses.device),
        )
    losses = torch.where(active, losses, torch.zeros_like(losses))
    return losses.sum() / torch.clamp(active.sum(), min=1)


def primary_validation_score(validation: dict[str, object]) -> float:
    event_metrics_by_name = validation["calibrated_event_metrics"]
    segmentation_metrics_by_name = validation["calibrated_segmentation_metrics"]
    primary_f1 = [
        float(event_metrics_by_name[event_name]["f1"])
        for event_name in ONSET_HEADS
    ]
    primary_f1.extend(
        float(segmentation_metrics_by_name[event_name]["f1"])
        for event_name in INTERVAL_HEADS
    )
    return float(np.mean(primary_f1))


def clone_state_dict(model: nn.Module) -> dict[str, torch.Tensor]:
    return {
        name: parameter.detach().cpu().clone()
        for name, parameter in model.state_dict().items()
    }


def configure_trainable_heads(
    model: BoundaryNet,
    train_heads: list[str] | None,
    fine_tune_shared_encoder: bool,
) -> None:
    if train_heads is None:
        return

    selected = {EVENT_NAMES.index(name) for name in train_heads}
    if not fine_tune_shared_encoder:
        for parameter in model.parameters():
            parameter.requires_grad = False
    model.output_heads.weight.requires_grad = True
    model.output_heads.bias.requires_grad = True

    mask = torch.zeros(len(EVENT_NAMES), dtype=torch.float32)
    for index in selected:
        mask[index] = 1.0

    def mask_weight_gradient(gradient: torch.Tensor) -> torch.Tensor:
        return gradient * mask.to(device=gradient.device, dtype=gradient.dtype).view(-1, 1, 1)

    def mask_bias_gradient(gradient: torch.Tensor) -> torch.Tensor:
        return gradient * mask.to(device=gradient.device, dtype=gradient.dtype)

    model.output_heads.weight.register_hook(mask_weight_gradient)
    model.output_heads.bias.register_hook(mask_bias_gradient)


def extract_event_frames(
    probabilities: np.ndarray, threshold: float, min_distance_frames: int
) -> np.ndarray:
    if probabilities.ndim != 1:
        raise ValueError("event probabilities must be one-dimensional")
    if probabilities.size == 0:
        return np.empty(0, dtype=np.int64)

    left = np.concatenate((np.asarray([-np.inf]), probabilities[:-1]))
    right = np.concatenate((probabilities[1:], np.asarray([-np.inf])))
    candidates = np.flatnonzero(
        (probabilities >= threshold)
        & (probabilities >= left)
        & (probabilities > right)
    )
    if candidates.size <= 1 or min_distance_frames <= 1:
        return candidates.astype(np.int64)

    selected: list[int] = []
    for candidate in sorted(
        candidates, key=lambda index: probabilities[index], reverse=True
    ):
        if all(
            abs(int(candidate) - existing) >= min_distance_frames
            for existing in selected
        ):
            selected.append(int(candidate))
    return np.asarray(sorted(selected), dtype=np.int64)


def extract_rising_edges(probabilities: np.ndarray, threshold: float) -> np.ndarray:
    if probabilities.ndim != 1:
        raise ValueError("interval probabilities must be one-dimensional")
    active = probabilities >= threshold
    previous = np.concatenate((np.asarray([False]), active[:-1]))
    return np.flatnonzero(active & ~previous).astype(np.int64)


def decode_event_frames(
    probabilities: np.ndarray,
    event_name: str,
    threshold: float,
    min_distance_frames: int,
) -> np.ndarray:
    if event_name in INTERVAL_HEADS:
        return extract_rising_edges(probabilities, threshold)
    return extract_event_frames(probabilities, threshold, min_distance_frames)


def match_events(
    predicted_frames: np.ndarray,
    expected_frames: np.ndarray,
    tolerance_frames: int,
) -> tuple[int, int, int]:
    predicted = np.sort(np.asarray(predicted_frames, dtype=np.int64))
    expected = np.sort(np.asarray(expected_frames, dtype=np.int64))
    predicted_index = 0
    expected_index = 0
    true_positive = 0
    false_positive = 0
    false_negative = 0

    while predicted_index < len(predicted) and expected_index < len(expected):
        prediction = predicted[predicted_index]
        target = expected[expected_index]
        if prediction < target - tolerance_frames:
            false_positive += 1
            predicted_index += 1
        elif prediction > target + tolerance_frames:
            false_negative += 1
            expected_index += 1
        else:
            true_positive += 1
            predicted_index += 1
            expected_index += 1

    false_positive += len(predicted) - predicted_index
    false_negative += len(expected) - expected_index
    return true_positive, false_positive, false_negative


def counts_to_metrics(
    true_positive: float, false_positive: float, false_negative: float
) -> dict[str, float | int]:
    precision = true_positive / max(1.0, true_positive + false_positive)
    recall = true_positive / max(1.0, true_positive + false_negative)
    f1 = 2.0 * precision * recall / max(1.0e-12, precision + recall)
    return {
        "precision": round(float(precision), 4),
        "recall": round(float(recall), 4),
        "f1": round(float(f1), 4),
        "true_positive": int(true_positive),
        "false_positive": int(false_positive),
        "false_negative": int(false_negative),
    }


def event_metrics(
    probabilities: list[np.ndarray],
    examples: list[Example],
    thresholds: tuple[float, ...],
    tolerance_frames: int,
    min_distance_frames: int,
) -> dict[str, dict[str, float | int]]:
    metrics = {}
    for event_index, event_name in enumerate(EVENT_NAMES):
        counts = np.zeros(3, dtype=np.int64)
        for example_probabilities, example in zip(
            probabilities, examples, strict=True
        ):
            predicted = decode_event_frames(
                example_probabilities[event_index],
                event_name,
                thresholds[event_index],
                min_distance_frames,
            )
            counts += match_events(
                predicted,
                example.event_frames[event_index],
                tolerance_frames,
            )
        metrics[event_name] = counts_to_metrics(*counts)
    return metrics


def segmentation_metrics(
    probabilities: list[np.ndarray],
    examples: list[Example],
    thresholds: tuple[float, ...],
) -> dict[str, dict[str, float | int]]:
    metrics = {}
    for event_name in INTERVAL_HEADS:
        event_index = EVENT_NAMES.index(event_name)
        counts = np.zeros(3, dtype=np.int64)
        for example_probabilities, example in zip(
            probabilities, examples, strict=True
        ):
            predicted = example_probabilities[event_index] >= thresholds[event_index]
            expected = example.targets[event_index] >= 0.5
            counts[0] += np.count_nonzero(predicted & expected)
            counts[1] += np.count_nonzero(predicted & ~expected)
            counts[2] += np.count_nonzero(~predicted & expected)
        metrics[event_name] = counts_to_metrics(*counts)
    return metrics


def calibrate_thresholds(
    probabilities: list[np.ndarray],
    examples: list[Example],
    tolerance_frames: int,
    min_distance_frames: int,
) -> tuple[tuple[float, ...], dict[str, object]]:
    threshold_grid = tuple(float(value) for value in np.arange(0.1, 0.91, 0.05))
    calibrated: list[float] = []
    for event_index in range(len(EVENT_NAMES)):
        event_name = EVENT_NAMES[event_index]
        if event_name in INTERVAL_HEADS:
            expected_count = sum(
                int(np.count_nonzero(example.targets[event_index]))
                for example in examples
            )
        else:
            expected_count = sum(
                len(example.event_frames[event_index]) for example in examples
            )
        if expected_count == 0:
            calibrated.append(0.5)
            continue

        best_threshold = 0.5
        best_score = (-1.0, -1.0, -1.0)
        for threshold in threshold_grid:
            thresholds = tuple(
                threshold if index == event_index else 0.5
                for index in range(len(EVENT_NAMES))
            )
            if event_name in INTERVAL_HEADS:
                metrics = segmentation_metrics(
                    probabilities,
                    examples,
                    thresholds,
                )[event_name]
            else:
                metrics = event_metrics(
                    probabilities,
                    examples,
                    thresholds,
                    tolerance_frames,
                    min_distance_frames,
                )[event_name]
            score = (
                float(metrics["f1"]),
                float(metrics["precision"]),
                -abs(threshold - 0.5),
            )
            if score > best_score:
                best_score = score
                best_threshold = threshold
        calibrated.append(round(best_threshold, 2))

    calibrated_thresholds = tuple(calibrated)
    return calibrated_thresholds, {
        "event_metrics": event_metrics(
            probabilities,
            examples,
            calibrated_thresholds,
            tolerance_frames,
            min_distance_frames,
        ),
        "segmentation_metrics": segmentation_metrics(
            probabilities,
            examples,
            calibrated_thresholds,
        ),
    }


def predict_examples(
    model: nn.Module, examples: list[Example], device: torch.device
) -> list[np.ndarray]:
    probabilities = []
    model.eval()
    with torch.no_grad():
        for example in examples:
            features = torch.from_numpy(example.features).unsqueeze(0).to(device)
            probabilities.append(
                torch.sigmoid(model(features)).squeeze(0).cpu().numpy()
            )
    return probabilities


def onset_frame_diagnostic(
    probabilities: list[np.ndarray], examples: list[Example]
) -> dict[str, dict[str, float | int]]:
    true_positive = np.zeros(len(ONSET_HEADS), dtype=np.float64)
    false_positive = np.zeros(len(ONSET_HEADS), dtype=np.float64)
    false_negative = np.zeros(len(ONSET_HEADS), dtype=np.float64)
    for example_probabilities, example in zip(probabilities, examples, strict=True):
        predictions = example_probabilities[: len(ONSET_HEADS)] >= 0.5
        expected = example.targets[: len(ONSET_HEADS)] >= 0.5
        true_positive += (predictions & expected).sum(axis=1)
        false_positive += (predictions & ~expected).sum(axis=1)
        false_negative += (~predictions & expected).sum(axis=1)

    metrics = {}
    for index, name in enumerate(ONSET_HEADS):
        metrics[name] = counts_to_metrics(
            true_positive[index], false_positive[index], false_negative[index]
        )
    return metrics


def evaluate(
    model: nn.Module,
    examples: list[Example],
    device: torch.device,
    event_tolerance_ms: float,
    event_min_distance_ms: float,
) -> dict[str, object]:
    probabilities = predict_examples(model, examples, device)
    tolerance_frames = max(
        0,
        int(round(event_tolerance_ms * TARGET_SAMPLE_RATE / (1000.0 * HOP_LENGTH))),
    )
    min_distance_frames = max(
        1,
        int(
            round(
                event_min_distance_ms
                * TARGET_SAMPLE_RATE
                / (1000.0 * HOP_LENGTH)
            )
        ),
    )
    fixed_thresholds = tuple(0.5 for _ in EVENT_NAMES)
    calibrated_thresholds, calibrated_metrics = calibrate_thresholds(
        probabilities,
        examples,
        tolerance_frames,
        min_distance_frames,
    )
    return {
        "event_tolerance_ms": event_tolerance_ms,
        "event_min_distance_ms": event_min_distance_ms,
        "fixed_threshold_event_metrics": event_metrics(
            probabilities,
            examples,
            fixed_thresholds,
            tolerance_frames,
            min_distance_frames,
        ),
        "fixed_threshold_segmentation_metrics": segmentation_metrics(
            probabilities,
            examples,
            fixed_thresholds,
        ),
        "calibrated_event_metrics": calibrated_metrics["event_metrics"],
        "calibrated_segmentation_metrics": calibrated_metrics[
            "segmentation_metrics"
        ],
        "calibrated_thresholds": dict(
            zip(EVENT_NAMES, calibrated_thresholds, strict=True)
        ),
        "threshold_calibration_objective": {
            "phoneme": "event_f1",
            "syllable": "event_f1",
            "breath": "segmentation_frame_f1",
            "silence": "segmentation_frame_f1",
        },
        "onset_frame_diagnostic_at_0_5": onset_frame_diagnostic(
            probabilities, examples
        ),
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Train a small log-mel CNN for GTSinger boundary onsets."
    )
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--dataset-root", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument(
        "--init-checkpoint",
        type=Path,
        help="initialize model weights from an existing compatible checkpoint",
    )
    parser.add_argument("--epochs", type=int, default=5)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--learning-rate", type=float, default=1.0e-3)
    parser.add_argument("--chunk-seconds", type=float, default=4.0)
    parser.add_argument("--label-tolerance-ms", type=float, default=30.0)
    parser.add_argument("--event-tolerance-ms", type=float, default=50.0)
    parser.add_argument("--event-min-distance-ms", type=float, default=30.0)
    parser.add_argument("--max-records", type=int)
    parser.add_argument("--max-steps-per-epoch", type=int)
    parser.add_argument(
        "--syllable-hard-negative-weight",
        type=float,
        default=1.0,
        help=(
            "extra loss weight for phoneme-positive, syllable-negative frames; "
            "values above 1 penalize consonant attacks misclassified as syllables"
        ),
    )
    parser.add_argument("--seed", type=int, default=20260613)
    parser.add_argument(
        "--use-manifest-syllable-labels",
        action="store_true",
        help="do not rederive corrected syllable proxy labels from annotation sidecars",
    )
    parser.add_argument(
        "--train-head",
        action="append",
        choices=EVENT_NAMES,
        help=(
            "restrict optimization to this output head; may be repeated. "
            "Useful for breath/silence fine-tunes that must not perturb syllables."
        ),
    )
    parser.add_argument(
        "--fine-tune-shared-encoder",
        action="store_true",
        help=(
            "when --train-head is used, keep the shared feature extractor trainable "
            "while masking gradients for unselected output heads"
        ),
    )
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.syllable_hard_negative_weight < 1.0:
        print("error: --syllable-hard-negative-weight must be >= 1", file=sys.stderr)
        return 2
    seed_everything(args.seed)
    records = load_manifest(args.manifest.expanduser().resolve())
    train_records = select_records(records, "train", args.max_records, args.seed)
    validation_records = select_records(
        records, "validation", args.max_records, args.seed
    )
    summary = {
        "manifest_records": len(records),
        "selected_train_records": len(train_records),
        "selected_validation_records": len(validation_records),
        "event_names": EVENT_NAMES,
        "head_tasks": HEAD_TASKS,
        "sample_rate": TARGET_SAMPLE_RATE,
        "hop_length": HOP_LENGTH,
        "frame_ms": 1000.0 * HOP_LENGTH / TARGET_SAMPLE_RATE,
        "encoder": "residual_dilated_tcn",
        "receptive_field_frames": receptive_field_frames(),
        "receptive_field_ms": (
            receptive_field_frames() * 1000.0 * HOP_LENGTH / TARGET_SAMPLE_RATE
        ),
        "syllable_label": "vowel-nucleus onset proxy",
        "syllable_label_source": (
            "manifest"
            if args.use_manifest_syllable_labels
            else "annotation_sidecar_rederived"
        ),
        "interval_target_rasterization": "half-open [start, end)",
        "syllable_hard_negative_weight": args.syllable_hard_negative_weight,
        "init_checkpoint": (
            str(args.init_checkpoint.expanduser().resolve())
            if args.init_checkpoint is not None
            else None
        ),
        "train_heads": args.train_head,
        "fine_tune_shared_encoder": args.fine_tune_shared_encoder,
    }
    if args.dry_run:
        print(json.dumps(summary, indent=2))
        return 0
    if not train_records:
        print("error: manifest has no selected training records", file=sys.stderr)
        return 1
    if args.output is None:
        print("error: --output is required unless --dry-run is used", file=sys.stderr)
        return 2

    dataset_root = args.dataset_root.expanduser().resolve()
    train_examples = load_examples(
        train_records,
        dataset_root,
        args.label_tolerance_ms,
        not args.use_manifest_syllable_labels,
    )
    validation_examples = load_examples(
        validation_records,
        dataset_root,
        args.label_tolerance_ms,
        not args.use_manifest_syllable_labels,
    )
    train_examples = restrict_examples_to_heads(train_examples, args.train_head)
    validation_examples = restrict_examples_to_heads(validation_examples, args.train_head)
    chunk_frames = max(
        1,
        int(round(args.chunk_seconds * TARGET_SAMPLE_RATE / HOP_LENGTH)),
    )
    train_dataset = ChunkDataset(train_examples, chunk_frames)
    generator = torch.Generator().manual_seed(args.seed)
    train_loader = DataLoader(
        train_dataset,
        batch_size=args.batch_size,
        shuffle=True,
        generator=generator,
    )
    device = torch.device(
        "mps"
        if torch.backends.mps.is_available()
        else "cuda"
        if torch.cuda.is_available()
        else "cpu"
    )
    model = BoundaryNet(N_MELS).to(device)
    init_thresholds = None
    if args.init_checkpoint is not None:
        init_checkpoint = torch.load(
            args.init_checkpoint.expanduser().resolve(),
            map_location="cpu",
            weights_only=True,
        )
        if init_checkpoint.get("model", {}).get("type") != "BoundaryNetResidualTCN":
            print("error: --init-checkpoint has unsupported model type", file=sys.stderr)
            return 2
        model.load_state_dict(init_checkpoint["model_state"])
        init_thresholds = init_checkpoint.get("training", {}).get(
            "calibrated_thresholds"
        )
    weights = positive_weights(train_examples).to(device)
    configure_trainable_heads(model, args.train_head, args.fine_tune_shared_encoder)
    optimizer = torch.optim.Adam(
        (parameter for parameter in model.parameters() if parameter.requires_grad),
        lr=args.learning_rate,
    )
    history = []
    calibrated_thresholds = (
        {name: float(init_thresholds[name]) for name in EVENT_NAMES}
        if isinstance(init_thresholds, dict)
        and all(name in init_thresholds for name in EVENT_NAMES)
        else {name: 0.5 for name in EVENT_NAMES}
    )
    best_state = clone_state_dict(model)
    best_epoch = 0
    best_validation_score = float("-inf")
    best_thresholds = calibrated_thresholds

    for epoch in range(1, args.epochs + 1):
        model.train()
        losses = []
        for step, (features, targets) in enumerate(train_loader, start=1):
            optimizer.zero_grad(set_to_none=True)
            device_targets = targets.to(device)
            logits = model(features.to(device))
            loss = training_loss(
                logits,
                device_targets,
                weights,
                args.syllable_hard_negative_weight,
            )
            loss.backward()
            optimizer.step()
            losses.append(float(loss.detach().cpu()))
            if (
                args.max_steps_per_epoch is not None
                and step >= args.max_steps_per_epoch
            ):
                break
        epoch_result = {
            "epoch": epoch,
            "train_loss": round(float(np.mean(losses)), 6),
        }
        if validation_examples:
            epoch_result["validation"] = evaluate(
                model,
                validation_examples,
                device,
                args.event_tolerance_ms,
                args.event_min_distance_ms,
            )
            calibrated_thresholds = epoch_result["validation"][
                "calibrated_thresholds"
            ]
            validation_score = primary_validation_score(epoch_result["validation"])
            epoch_result["primary_validation_macro_f1"] = round(
                validation_score, 6
            )
            if validation_score > best_validation_score:
                best_validation_score = validation_score
                best_epoch = epoch
                best_thresholds = dict(calibrated_thresholds)
                if args.train_head is not None and isinstance(init_thresholds, dict):
                    for event_name in EVENT_NAMES:
                        if event_name not in args.train_head and event_name in init_thresholds:
                            best_thresholds[event_name] = float(init_thresholds[event_name])
                best_state = clone_state_dict(model)
        else:
            best_epoch = epoch
            best_state = clone_state_dict(model)
            best_thresholds = dict(calibrated_thresholds)
        history.append(epoch_result)
        print(json.dumps(epoch_result), file=sys.stderr)

    output = args.output.expanduser().resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    checkpoint = {
        "model_state": best_state,
        "model": {
            "type": "BoundaryNetResidualTCN",
            "encoder": {
                "type": "residual_dilated_tcn",
                "channels": TCN_CHANNELS,
                "kernel_size": TCN_KERNEL_SIZE,
                "dilations": TCN_DILATIONS,
                "convolutions_per_block": 1,
                "receptive_field_frames": receptive_field_frames(),
                "receptive_field_ms": (
                    receptive_field_frames()
                    * 1000.0
                    * HOP_LENGTH
                    / TARGET_SAMPLE_RATE
                ),
            },
            "event_names": EVENT_NAMES,
            "head_tasks": HEAD_TASKS,
            "output_semantics": {
                "phoneme": "onset_probability",
                "syllable": "onset_probability",
                "breath": "inside_interval_probability",
                "silence": "inside_interval_probability",
            },
            "sample_rate": TARGET_SAMPLE_RATE,
            "hop_length": HOP_LENGTH,
            "n_fft": N_FFT,
            "n_mels": N_MELS,
        },
        "training": {
            **summary,
            "seed": args.seed,
            "epochs": args.epochs,
            "label_tolerance_ms": args.label_tolerance_ms,
            "event_tolerance_ms": args.event_tolerance_ms,
            "event_min_distance_ms": args.event_min_distance_ms,
            "class_weighting": {
                "onset_heads": "inverse_frequency",
                "interval_heads": "sqrt_inverse_frequency",
                "syllable_hard_negatives": args.syllable_hard_negative_weight,
            },
            "calibrated_thresholds": best_thresholds,
            "positive_weights": weights.cpu().tolist(),
            "best_epoch": best_epoch,
            "best_validation_macro_f1": (
                round(best_validation_score, 6)
                if np.isfinite(best_validation_score)
                else None
            ),
            "checkpoint_selection": (
                "macro mean of phoneme/syllable event F1 and "
                "breath/silence segmentation F1"
            ),
            "final_epoch": args.epochs,
            "history": history,
        },
    }
    torch.save(checkpoint, output)
    print(json.dumps({**summary, "device": str(device), "output": str(output)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
