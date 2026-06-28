#!/usr/bin/env python3
"""Preferred offline note detector for Synthetic Obsidian.

Order of preference:
1. Local hybrid ML note detector checkpoint (`nvt.note_detector`).
2. Legacy torchcrepe + heuristic segmentation fallback.

The output format remains stable for the JUCE bridge:
{"notes": [{"pitch", "start", "duration", "confidence", "cents"}]}
"""

from __future__ import annotations

import argparse
import json
import os
import importlib.util
import sys
from pathlib import Path

import librosa
import numpy as np
import torch
import torchcrepe


def _project_root() -> Path:
    here = Path(__file__).resolve()
    return here.parents[2]


def _research_root() -> Path:
    return _project_root() / "research"


def _hybrid_checkpoint() -> Path:
    root = _research_root()
    candidates = [
        root / "checkpoints_note_detector_finetune" / "best.pt",
        root / "checkpoints_note_detector_candidates" / "best.pt",
        root / "checkpoints_note_detector_cls" / "best.pt",
        root / "checkpoints_note_detector_boundary" / "best.pt",
        root / "checkpoints_note_detector" / "best.pt",
    ]
    for candidate in candidates:
        if candidate.exists():
            return candidate
    return candidates[0]


def _midi_to_hz(midi_pitch: float) -> float:
    return 440.0 * (2.0 ** ((midi_pitch - 69.0) / 12.0))


def _run_pyin_backend(path: Path, sensitivity: float) -> list[dict] | None:
    script_path = Path(__file__).with_name("analyze_notes_pyin.py")
    if not script_path.exists():
        return None

    spec = importlib.util.spec_from_file_location("analyze_notes_pyin", script_path)
    if spec is None or spec.loader is None:
        return None

    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    analyze_fn = getattr(module, "analyze", None)
    if analyze_fn is None:
        return None

    return analyze_fn(path, sensitivity)


def _hybrid_sensitivity_params(sensitivity: float) -> dict[str, float]:
    sensitivity = float(np.clip(sensitivity, 0.0, 1.0))
    return {
        "voiced_threshold": 0.54 - 0.05 * sensitivity,
        "hybrid_threshold": 0.30 - 0.06 * sensitivity,
        "pitch_jump_semitones": 1.22 - 0.10 * sensitivity,
        "min_note_seconds": 0.125 - 0.03 * sensitivity,
        "min_same_pitch_split_seconds": 0.20 - 0.03 * sensitivity,
        "median_window": 5,
    }


def _run_hybrid_model(path: Path, sensitivity: float) -> list[dict] | None:
    checkpoint = _hybrid_checkpoint()
    if not checkpoint.exists():
        return None

    research_root = _research_root()
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

    audio, clip_start, _clip_end = load_audio_chunk(path, 0.0, 120.0, sample_rate)
    log_mel = make_log_mel(audio, sample_rate, n_fft, hop_length, n_mels).unsqueeze(0).to(device)

    with torch.no_grad():
        predictions = model(log_mel)

    voiced_probs = torch.sigmoid(predictions["voiced_logits"]).squeeze(0).cpu()
    onset_probs = torch.sigmoid(predictions["onset_logits"]).squeeze(0).cpu()
    pitch_midi = pitch_logits_to_midi(predictions["pitch_logits"].squeeze(0).cpu(), midi_min)
    audio_onset, audio_flux = build_audio_boundary_features(audio, sample_rate, hop_length)
    audio_onset = pad_or_trim_feature(audio_onset, len(voiced_probs))
    audio_flux = pad_or_trim_feature(audio_flux, len(voiced_probs))

    params = _hybrid_sensitivity_params(sensitivity)
    notes = decode_notes(
        voiced_probs=voiced_probs,
        onset_probs=onset_probs,
        pitch_midi=pitch_midi,
        hop_seconds=hop_length / sample_rate,
        clip_start=clip_start,
        voiced_threshold=params["voiced_threshold"],
        onset_threshold=params["hybrid_threshold"],
        pitch_jump_semitones=params["pitch_jump_semitones"],
        min_note_seconds=params["min_note_seconds"],
        median_window=int(params["median_window"]),
        audio_onset=audio_onset,
        audio_flux=audio_flux,
        ml_onset_weight=0.25,
        audio_onset_weight=0.55,
        audio_flux_weight=0.20,
        min_same_pitch_split_seconds=params["min_same_pitch_split_seconds"],
    )
    notes = cleanup_decoded_notes(notes, params["min_note_seconds"])

    output: list[dict] = []
    for note in notes:
        pitch_float = float(note["pitch"])
        rounded_pitch = int(np.clip(np.rint(pitch_float), 0, 127))
        cents = float(np.clip((pitch_float - rounded_pitch) * 100.0, -50.0, 50.0))
        output.append(
            {
                "pitch": rounded_pitch,
                "start": float(note["start"]),
                "duration": float(note["duration"]),
                "confidence": float(np.clip(note["confidence"], 0.0, 1.0)),
                "cents": cents,
            }
        )

    return output


def _fill_short_gaps(midi: np.ndarray, conf: np.ndarray, max_gap_frames: int) -> np.ndarray:
    result = midi.copy()
    i = 0
    while i < len(result):
        if result[i] >= 0:
            i += 1
            continue

        start = i
        while i < len(result) and result[i] < 0:
            i += 1

        end = i - 1
        left = start - 1
        right = i
        if left < 0 or right >= len(result) or end - start + 1 > max_gap_frames:
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


def _median_smooth(midi: np.ndarray, radius: int = 5) -> np.ndarray:
    result = midi.copy()
    for idx, pitch in enumerate(midi):
        if pitch < 0:
            continue
        voiced = midi[max(0, idx - radius) : min(len(midi), idx + radius + 1)]
        voiced = voiced[voiced >= 0]
        if len(voiced) >= 3:
            result[idx] = int(np.median(voiced))
    return result


def _onset_strength_at(
    onset_strength: np.ndarray,
    center_idx: int,
    radius: int = 2,
) -> float:
    if onset_strength.size == 0:
        return 0.0
    first = max(0, center_idx - radius)
    last = min(len(onset_strength), center_idx + radius + 1)
    if first >= last:
        return 0.0
    return float(np.max(onset_strength[first:last]))


def _merge_notes(
    midi: np.ndarray,
    f0: np.ndarray,
    conf: np.ndarray,
    onset_strength: np.ndarray,
    hop_sec: float,
    min_note_sec: float = 0.075,
    merge_gap_sec: float = 0.36,
    onset_split_threshold: float = 0.62,
) -> list[dict]:
    notes: list[dict] = []
    current = -1
    start_idx = 0
    last_idx = 0
    pitches: list[int] = []
    freqs: list[float] = []
    max_conf = 0.0

    def commit(end_idx: int) -> None:
        nonlocal current, start_idx, pitches, freqs, max_conf
        duration = (end_idx - start_idx + 1) * hop_sec
        if current < 0 or duration < min_note_sec or not pitches:
            return

        values, counts = np.unique(np.asarray(pitches), return_counts=True)
        dominant = int(values[int(np.argmax(counts))])
        avg_hz = float(np.mean(freqs)) if freqs else 0.0
        cents = 0.0
        if avg_hz > 0.0:
            cents = float(np.clip(1200.0 * np.log2(avg_hz / _midi_to_hz(dominant)), -50.0, 50.0))

        notes.append(
            {
                "pitch": dominant,
                "start": float(start_idx * hop_sec),
                "duration": float(duration),
                "confidence": float(np.clip(max_conf, 0.0, 1.0)),
                "cents": cents,
            }
        )

    max_gap_frames = max(1, int(round(merge_gap_sec / hop_sec)))
    for idx, pitch in enumerate(midi):
        pitched = pitch >= 0
        same_pitch = pitched and current >= 0 and abs(int(pitch) - current) <= 1
        gap_ok = idx - last_idx <= max_gap_frames
        current_duration = (idx - start_idx + 1) * hop_sec
        split_on_onset = (
            same_pitch
            and current_duration >= 0.16
            and _onset_strength_at(onset_strength, idx) >= onset_split_threshold
        )

        if same_pitch and gap_ok and not split_on_onset:
            last_idx = idx
            max_conf = max(max_conf, float(conf[idx]))
            pitches.append(int(pitch))
            if f0[idx] > 0.0:
                freqs.append(float(f0[idx]))
            continue

        if current >= 0:
            commit(last_idx)

        if pitched:
            current = int(pitch)
            start_idx = idx
            last_idx = idx
            max_conf = float(conf[idx])
            pitches = [int(pitch)]
            freqs = [float(f0[idx])] if f0[idx] > 0.0 else []
        else:
            current = -1
            pitches = []
            freqs = []
            max_conf = 0.0

    if current >= 0:
        commit(last_idx)

    return notes


def _cleanup_notes(notes: list[dict], sensitivity: float) -> list[dict]:
    if len(notes) < 2:
        return notes

    min_transition_sec = 0.055 + 0.05 * (1.0 - sensitivity)
    max_absorb_gap = 0.14 + 0.08 * sensitivity
    cleaned: list[dict] = []

    for note in notes:
        if not cleaned:
            cleaned.append(note)
            continue

        prev = cleaned[-1]
        prev_end = float(prev["start"]) + float(prev["duration"])
        gap = float(note["start"]) - prev_end
        same_pitch = abs(int(note["pitch"]) - int(prev["pitch"])) <= 1

        if same_pitch and gap <= max_absorb_gap:
            new_end = max(prev_end, float(note["start"]) + float(note["duration"]))
            prev["duration"] = new_end - float(prev["start"])
            prev["confidence"] = max(float(prev["confidence"]), float(note["confidence"]))
            continue

        if float(note["duration"]) < min_transition_sec and gap <= max_absorb_gap:
            new_end = max(prev_end, float(note["start"]) + float(note["duration"]))
            prev["duration"] = new_end - float(prev["start"])
            prev["confidence"] = max(float(prev["confidence"]), float(note["confidence"]) * 0.8)
            continue

        cleaned.append(note)

    return cleaned


def _legacy_analyze(path: Path, model: str, device: str, sensitivity: float) -> list[dict]:
    sample_rate = 16000
    hop_length = 160
    sensitivity = float(np.clip(sensitivity, 0.0, 1.0))
    confidence_threshold = 0.22 - 0.17 * sensitivity
    min_note_sec = 0.12 - 0.07 * sensitivity
    merge_gap_sec = 0.24 + 0.24 * sensitivity
    fill_gap_sec = 0.35 + 0.45 * sensitivity
    onset_split_threshold = 0.72 - 0.34 * sensitivity

    y, sr = librosa.load(path, sr=sample_rate, mono=True, duration=120.0)
    if y.size == 0:
        return []

    wav = torch.from_numpy(y.astype(np.float32)).unsqueeze(0)
    f0_t, conf_t = torchcrepe.predict(
        wav,
        sr,
        hop_length,
        65.0,
        1100.0,
        model=model,
        return_periodicity=True,
        device=torch.device(device),
        batch_size=512,
    )

    f0 = f0_t.squeeze(0).detach().cpu().numpy().astype(np.float32)
    conf = conf_t.squeeze(0).detach().cpu().numpy().astype(np.float32)

    midi = np.full(len(f0), -1, dtype=np.int32)
    valid = (f0 > 0.0) & (conf >= confidence_threshold)
    midi[valid] = np.rint(librosa.hz_to_midi(f0[valid])).astype(np.int32)

    hop_sec = hop_length / sr
    midi = _fill_short_gaps(midi, conf, max_gap_frames=int(round(fill_gap_sec / hop_sec)))
    midi = _median_smooth(midi, radius=5)
    onset_strength = librosa.onset.onset_strength(
        y=y,
        sr=sr,
        hop_length=hop_length,
        aggregate=np.median,
    ).astype(np.float32)
    if onset_strength.size:
        peak = float(np.max(onset_strength))
        if peak > 1.0e-6:
            onset_strength = onset_strength / peak

    notes = _merge_notes(
        midi,
        f0,
        conf,
        onset_strength,
        hop_sec,
        min_note_sec=min_note_sec,
        merge_gap_sec=merge_gap_sec,
        onset_split_threshold=onset_split_threshold,
    )
    return _cleanup_notes(notes, sensitivity)


def analyze(path: Path, model: str, device: str, sensitivity: float) -> list[dict]:
    try:
        pyin_notes = _run_pyin_backend(path, sensitivity)
        if pyin_notes:
            return pyin_notes
    except Exception:
        pass

    use_experimental = os.environ.get("SO_USE_EXPERIMENTAL_NOTE_DETECTOR", "").strip() == "1"
    if use_experimental:
        try:
            hybrid = _run_hybrid_model(path, sensitivity)
            if hybrid:
                return hybrid
        except Exception:
            pass
    return _legacy_analyze(path, model, device, sensitivity)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("audio_file", type=Path)
    parser.add_argument("--model", choices=("tiny", "full"), default="full")
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--sensitivity", type=float, default=0.72)
    args = parser.parse_args()
    print(json.dumps({"notes": analyze(args.audio_file, args.model, args.device, args.sensitivity)}, ensure_ascii=False))


if __name__ == "__main__":
    main()
