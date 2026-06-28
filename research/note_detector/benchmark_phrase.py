#!/usr/bin/env python3
"""Benchmark note-detector predictions against a short reference MIDI phrase."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

import pretty_midi
import torch


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Compare predicted notes with a reference MIDI phrase.")
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--audio", type=Path, required=True)
    parser.add_argument("--midi", type=Path, required=True)
    parser.add_argument("--start", type=float, required=True)
    parser.add_argument("--end", type=float, required=True)
    parser.add_argument(
        "--midi-shift-seconds",
        type=float,
        default=0.0,
        help="Shift reference MIDI notes by this amount before clipping, useful for excerpted audio.",
    )
    parser.add_argument("--output-json", type=Path, default=None)
    return parser.parse_args()


def load_reference_notes(midi_path: Path, start: float, end: float, midi_shift_seconds: float = 0.0) -> list[dict[str, float]]:
    midi = pretty_midi.PrettyMIDI(str(midi_path))
    notes = sorted((note for instrument in midi.instruments for note in instrument.notes), key=lambda note: note.start)
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
        output.append(
            {
                "pitch": float(note.pitch),
                "start": clipped_start,
                "duration": clipped_end - clipped_start,
            }
        )
    return output


def load_predictions(
    checkpoint: Path,
    audio: Path,
    start: float,
    end: float,
) -> list[dict[str, float]]:
    research_root = Path(__file__).resolve().parents[1]
    if str(research_root) not in sys.path:
        sys.path.insert(0, str(research_root))

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

    audio_chunk, clip_start, _clip_end = load_audio_chunk(audio, start, end, sample_rate)
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
        voiced_threshold=0.51,
        onset_threshold=0.26,
        pitch_jump_semitones=1.15,
        min_note_seconds=0.11,
        median_window=5,
        audio_onset=audio_onset,
        audio_flux=audio_flux,
        ml_onset_weight=0.25,
        audio_onset_weight=0.55,
        audio_flux_weight=0.20,
        min_same_pitch_split_seconds=0.18,
    )
    return cleanup_decoded_notes(notes, min_note_seconds=0.11)


def overlap_seconds(left: dict[str, float], right: dict[str, float]) -> float:
    left_end = float(left["start"]) + float(left["duration"])
    right_end = float(right["start"]) + float(right["duration"])
    return max(0.0, min(left_end, right_end) - max(float(left["start"]), float(right["start"])))


def note_end(note: dict[str, float]) -> float:
    return float(note["start"]) + float(note["duration"])


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
    matches: list[dict[str, float | int]] = []

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
            pitch_error = abs(float(ref_note["pitch"]) - float(pred_note["pitch"]))
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
        best_overlap = overlap_seconds(ref_note, pred_note)
        ov_ratio = overlap_ratio(ref_note, pred_note)
        start_error = abs(float(ref_note["start"]) - float(pred_note["start"]))
        duration_error = abs(float(ref_note["duration"]) - float(pred_note["duration"]))
        pitch_error = abs(float(ref_note["pitch"]) - float(pred_note["pitch"]))
        octave_norm_error = octave_normalized_pitch_error(float(ref_note["pitch"]), float(pred_note["pitch"]))
        allowed_pitch_error = pitch_tolerance(min(float(ref_note["duration"]), float(pred_note["duration"])))
        if ov_ratio < 0.33 and start_error > 0.18:
            continue
        unmatched_predicted.discard(best_index)
        matches.append(
            {
                "ref_index": ref_index,
                "pred_index": best_index,
                "overlap": best_overlap,
                "overlap_ratio": ov_ratio,
                "pitch_error": pitch_error,
                "octave_normalized_pitch_error": octave_norm_error,
                "pitch_within_tolerance": pitch_error <= allowed_pitch_error,
                "octave_normalized_pitch_within_tolerance": octave_norm_error <= allowed_pitch_error,
                "pitch_tolerance": allowed_pitch_error,
                "start_error": start_error,
                "duration_error": duration_error,
            }
        )

    def mean(values: list[float]) -> float | None:
        if not values:
            return None
        return float(sum(values) / len(values))

    overlap_ratios = [float(item["overlap_ratio"]) for item in matches]
    pitch_errors = [float(item["pitch_error"]) for item in matches]
    octave_norm_pitch_errors = [float(item["octave_normalized_pitch_error"]) for item in matches]
    start_errors = [float(item["start_error"]) for item in matches]
    duration_errors = [float(item["duration_error"]) for item in matches]
    pitch_within_tolerance = sum(1 for item in matches if bool(item["pitch_within_tolerance"]))
    octave_norm_pitch_within_tolerance = sum(
        1 for item in matches if bool(item["octave_normalized_pitch_within_tolerance"])
    )
    unmatched_reference = len(reference) - len(matches)
    unmatched_predicted_count = len(unmatched_predicted)
    return {
        "reference_count": len(reference),
        "predicted_count": len(predicted),
        "intent_match_count": len(matches),
        "unmatched_reference": unmatched_reference,
        "unmatched_predicted": unmatched_predicted_count,
        "split_excess": max(0, unmatched_predicted_count),
        "merge_misses": max(0, unmatched_reference),
        "overlap_ratio_mean": mean(overlap_ratios),
        "pitch_error_mean": mean(pitch_errors),
        "octave_normalized_pitch_error_mean": mean(octave_norm_pitch_errors),
        "pitch_tolerant_accuracy": (pitch_within_tolerance / len(matches)) if matches else None,
        "octave_normalized_pitch_tolerant_accuracy": (
            octave_norm_pitch_within_tolerance / len(matches)
        ) if matches else None,
        "start_error_mean": mean(start_errors),
        "duration_error_mean": mean(duration_errors),
        "matches": matches,
    }


def main() -> int:
    args = parse_args()
    reference = load_reference_notes(args.midi, args.start, args.end, args.midi_shift_seconds)
    predicted = load_predictions(args.checkpoint, args.audio, args.start, args.end)
    result = {
        "audio": str(args.audio),
        "midi": str(args.midi),
        "checkpoint": str(args.checkpoint),
        "window": {"start": args.start, "end": args.end},
        "midi_shift_seconds": args.midi_shift_seconds,
        "reference_notes": reference,
        "predicted_notes": predicted,
        "summary": compare(reference, predicted),
    }
    if args.output_json is not None:
        args.output_json.write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result["summary"], ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
