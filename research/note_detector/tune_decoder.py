#!/usr/bin/env python3
"""Grid-search decoder parameters on a small phrase benchmark set."""

from __future__ import annotations

import argparse
import itertools
import json
import sys
from pathlib import Path
from typing import Any

import torch


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Tune note-decoder parameters on labeled phrase snippets.")
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--config-json", type=Path, required=True, help="JSON array of benchmark phrase configs.")
    parser.add_argument("--output-json", type=Path, default=None)
    return parser.parse_args()


def ensure_research_path() -> None:
    research_root = Path(__file__).resolve().parents[1]
    if str(research_root) not in sys.path:
        sys.path.insert(0, str(research_root))


def load_reference_notes(midi_path: Path, start: float, end: float, midi_shift_seconds: float) -> list[dict[str, float]]:
    import pretty_midi

    midi = pretty_midi.PrettyMIDI(str(midi_path))
    notes = sorted((note for inst in midi.instruments for note in inst.notes), key=lambda note: note.start)
    output: list[dict[str, float]] = []
    for note in notes:
        shifted_start = float(note.start) - midi_shift_seconds
        shifted_end = float(note.end) - midi_shift_seconds
        if shifted_end < start or shifted_start > end:
            continue
        clipped_start = max(start, shifted_start)
        clipped_end = min(end, shifted_end)
        if clipped_end <= clipped_start:
            continue
        output.append({"pitch": float(note.pitch), "start": clipped_start, "duration": clipped_end - clipped_start})
    return output


def load_model_predictions(
    checkpoint: Path,
    audio: Path,
    start: float,
    end: float,
    params: dict[str, float],
) -> list[dict[str, float]]:
    ensure_research_path()
    from nvt.note_detector.infer import (  # type: ignore
        build_audio_boundary_features,
        cleanup_decoded_notes,
        decode_notes,
        load_audio_chunk,
        load_checkpoint,
        make_log_mel,
        pad_or_trim_feature,
        pitch_logits_to_midi,
    )

    device = torch.device("cpu")
    model, train_args = load_checkpoint(checkpoint, device)
    sample_rate = int(train_args.get("sample_rate", 24000))
    hop_length = int(train_args.get("hop_length", 256))
    n_fft = int(train_args.get("n_fft", 1024))
    n_mels = int(train_args.get("n_mels", 80))
    midi_min = int(train_args.get("midi_min", 36))

    audio_chunk, clip_start, _ = load_audio_chunk(audio, start, end, sample_rate)
    log_mel = make_log_mel(audio_chunk, sample_rate, n_fft, hop_length, n_mels).unsqueeze(0).to(device)
    with torch.no_grad():
        predictions = model(log_mel)

    voiced_probs = torch.sigmoid(predictions["voiced_logits"]).squeeze(0).cpu()
    onset_probs = torch.sigmoid(predictions["onset_logits"]).squeeze(0).cpu()
    pitch_midi = pitch_logits_to_midi(predictions["pitch_logits"].squeeze(0).cpu(), midi_min)
    audio_onset, audio_flux = build_audio_boundary_features(audio_chunk, sample_rate, hop_length)
    audio_onset = pad_or_trim_feature(audio_onset, len(voiced_probs))
    audio_flux = pad_or_trim_feature(audio_flux, len(voiced_probs))

    notes = decode_notes(
        voiced_probs=voiced_probs,
        onset_probs=onset_probs,
        pitch_midi=pitch_midi,
        hop_seconds=hop_length / sample_rate,
        clip_start=clip_start,
        voiced_threshold=float(params["voiced_threshold"]),
        onset_threshold=float(params["onset_threshold"]),
        pitch_jump_semitones=float(params["pitch_jump_semitones"]),
        min_note_seconds=float(params["min_note_seconds"]),
        median_window=5,
        audio_onset=audio_onset,
        audio_flux=audio_flux,
        ml_onset_weight=0.25,
        audio_onset_weight=0.55,
        audio_flux_weight=0.20,
        min_same_pitch_split_seconds=float(params["min_same_pitch_split_seconds"]),
    )
    return cleanup_decoded_notes(notes, min_note_seconds=float(params["min_note_seconds"]))


def overlap_seconds(left: dict[str, float], right: dict[str, float]) -> float:
    left_end = float(left["start"]) + float(left["duration"])
    right_end = float(right["start"]) + float(right["duration"])
    return max(0.0, min(left_end, right_end) - max(float(left["start"]), float(right["start"])))


def overlap_ratio(left: dict[str, float], right: dict[str, float]) -> float:
    overlap = overlap_seconds(left, right)
    denom = max(min(float(left["duration"]), float(right["duration"])), 1.0e-6)
    return overlap / denom


def pitch_tolerance(duration_seconds: float) -> float:
    if duration_seconds <= 0.14:
        return 2.5
    if duration_seconds <= 0.24:
        return 1.5
    return 1.0


def octave_normalized_pitch_error(reference_pitch: float, predicted_pitch: float) -> float:
    deltas = [abs(reference_pitch - (predicted_pitch + 12.0 * shift)) for shift in range(-3, 4)]
    return float(min(deltas))


def compare(reference: list[dict[str, float]], predicted: list[dict[str, float]]) -> dict[str, Any]:
    unmatched_predicted = set(range(len(predicted)))
    matches: list[dict[str, float | int | bool]] = []
    for ref_index, ref_note in enumerate(reference):
        best_index = None
        best_score = -1.0
        for pred_index in sorted(unmatched_predicted):
            pred_note = predicted[pred_index]
            ov = overlap_seconds(ref_note, pred_note)
            if ov <= 0.0:
                continue
            start_error = abs(float(ref_note["start"]) - float(pred_note["start"]))
            ov_ratio = overlap_ratio(ref_note, pred_note)
            octave_norm_error = octave_normalized_pitch_error(float(ref_note["pitch"]), float(pred_note["pitch"]))
            allowed_pitch_error = pitch_tolerance(min(float(ref_note["duration"]), float(pred_note["duration"])))
            pitch_credit = max(0.0, 1.0 - (octave_norm_error / max(allowed_pitch_error * 2.0, 1.0)))
            score = 0.60 * ov_ratio + 0.25 * pitch_credit + 0.15 * max(0.0, 1.0 - start_error / 0.18)
            if score > best_score:
                best_score = score
                best_index = pred_index
        if best_index is None:
            continue
        pred_note = predicted[best_index]
        ov_ratio = overlap_ratio(ref_note, pred_note)
        start_error = abs(float(ref_note["start"]) - float(pred_note["start"]))
        if ov_ratio < 0.33 and start_error > 0.18:
            continue
        unmatched_predicted.discard(best_index)
        err = octave_normalized_pitch_error(float(ref_note["pitch"]), float(pred_note["pitch"]))
        tol = pitch_tolerance(min(float(ref_note["duration"]), float(pred_note["duration"])))
        matches.append(
            {
                "ref_index": ref_index,
                "pred_index": best_index,
                "overlap_ratio": ov_ratio,
                "octave_normalized_pitch_error": err,
                "pitch_ok": err <= tol,
                "start_error": start_error,
                "duration_error": abs(float(ref_note["duration"]) - float(pred_note["duration"])),
            }
        )

    def mean(values: list[float]) -> float:
        return float(sum(values) / max(1, len(values)))

    overlap_ratios = [float(item["overlap_ratio"]) for item in matches]
    octave_norm_pitch_errors = [float(item["octave_normalized_pitch_error"]) for item in matches]
    start_errors = [float(item["start_error"]) for item in matches]
    duration_errors = [float(item["duration_error"]) for item in matches]
    pitch_ok = sum(1 for item in matches if bool(item["pitch_ok"]))
    return {
        "reference_count": len(reference),
        "predicted_count": len(predicted),
        "intent_match_count": len(matches),
        "split_excess": len(unmatched_predicted),
        "merge_misses": len(reference) - len(matches),
        "overlap_ratio_mean": mean(overlap_ratios),
        "octave_normalized_pitch_error_mean": mean(octave_norm_pitch_errors),
        "octave_normalized_pitch_tolerant_accuracy": pitch_ok / max(1, len(matches)),
        "start_error_mean": mean(start_errors),
        "duration_error_mean": mean(duration_errors),
    }


def score_summary(summary: dict[str, Any]) -> float:
    return (
        2.5 * float(summary["merge_misses"])
        + 1.5 * float(summary["split_excess"])
        - 0.75 * float(summary["intent_match_count"])
        - 2.0 * float(summary["octave_normalized_pitch_tolerant_accuracy"])
        + 2.0 * float(summary["start_error_mean"])
        + 0.5 * float(summary["duration_error_mean"])
    )


def aggregate(summaries: list[dict[str, Any]]) -> dict[str, Any]:
    keys = [
        "reference_count",
        "predicted_count",
        "intent_match_count",
        "split_excess",
        "merge_misses",
        "overlap_ratio_mean",
        "octave_normalized_pitch_error_mean",
        "octave_normalized_pitch_tolerant_accuracy",
        "start_error_mean",
        "duration_error_mean",
    ]
    result: dict[str, Any] = {}
    for key in keys:
        values = [float(item[key]) for item in summaries]
        result[key] = float(sum(values) / len(values))
    return result


def main() -> int:
    args = parse_args()
    config = json.loads(args.config_json.read_text())
    param_grid = {
        "voiced_threshold": [0.49, 0.51, 0.53],
        "onset_threshold": [0.26, 0.29, 0.32],
        "pitch_jump_semitones": [1.15, 1.25, 1.4],
        "min_note_seconds": [0.08, 0.09, 0.11],
        "min_same_pitch_split_seconds": [0.18, 0.20, 0.24],
    }

    best: dict[str, Any] | None = None
    trials: list[dict[str, Any]] = []
    for values in itertools.product(*param_grid.values()):
        params = dict(zip(param_grid.keys(), values))
        sample_results = []
        for sample in config:
            reference = load_reference_notes(
                Path(sample["midi"]),
                float(sample["start"]),
                float(sample["end"]),
                float(sample.get("midi_shift_seconds", 0.0)),
            )
            predicted = load_model_predictions(
                args.checkpoint,
                Path(sample["audio"]),
                float(sample["start"]),
                float(sample["end"]),
                params,
            )
            summary = compare(reference, predicted)
            sample_results.append({"name": sample["name"], "summary": summary})
        aggregated = aggregate([item["summary"] for item in sample_results])
        objective = score_summary(aggregated)
        row = {"params": params, "objective": objective, "aggregate": aggregated, "samples": sample_results}
        trials.append(row)
        if best is None or objective < float(best["objective"]):
            best = row

    assert best is not None
    result = {
        "checkpoint": str(args.checkpoint),
        "config_json": str(args.config_json),
        "best": best,
        "top5": sorted(trials, key=lambda item: float(item["objective"]))[:5],
    }
    if args.output_json is not None:
        args.output_json.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(best, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
