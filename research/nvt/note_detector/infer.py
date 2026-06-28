from __future__ import annotations

import argparse
import json
import statistics
from pathlib import Path
from typing import Any

import librosa
import numpy as np
import soundfile as sf
import torch
import torchaudio

from nvt.note_detector.model import NoteDetectorModel


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run baseline note-detector inference on audio or manifest chunks.")
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--audio", type=Path, default=None, help="Path to a source WAV file.")
    parser.add_argument("--start", type=float, default=0.0, help="Chunk start in seconds inside --audio.")
    parser.add_argument("--end", type=float, default=0.0, help="Chunk end in seconds inside --audio. 0 means full file.")
    parser.add_argument("--manifest-row", type=str, default="", help="Chunk id from manifest.jsonl for direct lookup.")
    parser.add_argument("--manifest", type=Path, default=Path("data/note_detector/local_segments/manifest.jsonl"))
    parser.add_argument("--output-json", type=Path, default=None)
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    parser.add_argument("--voiced-threshold", type=float, default=0.51)
    parser.add_argument("--onset-threshold", type=float, default=0.45)
    parser.add_argument("--pitch-jump-semitones", type=float, default=1.15)
    parser.add_argument("--min-note-seconds", type=float, default=0.11)
    parser.add_argument("--median-window", type=int, default=5)
    parser.add_argument("--decode-mode", choices=("ml", "hybrid"), default="hybrid")
    parser.add_argument("--hybrid-threshold", type=float, default=0.26)
    parser.add_argument("--audio-onset-weight", type=float, default=0.55)
    parser.add_argument("--audio-flux-weight", type=float, default=0.20)
    parser.add_argument("--ml-onset-weight", type=float, default=0.25)
    parser.add_argument("--min-same-pitch-split-seconds", type=float, default=0.18)
    return parser.parse_args()


def load_checkpoint(path: Path, device: torch.device) -> tuple[NoteDetectorModel, dict[str, Any]]:
    state = torch.load(str(path), map_location=device, weights_only=False)
    args = state.get("args", {})
    midi_min = int(args.get("midi_min", 36))
    midi_max = int(args.get("midi_max", 84))
    model = NoteDetectorModel(
        n_mels=int(args.get("n_mels", 80)),
        n_pitch_bins=midi_max - midi_min + 1,
    )
    model.load_state_dict(state["model"])
    model.to(device)
    model.eval()
    return model, args


def find_manifest_row(manifest_path: Path, chunk_id: str) -> dict[str, Any]:
    with manifest_path.open("r", encoding="utf-8") as handle:
        for line in handle:
            if not line.strip():
                continue
            row = json.loads(line)
            if row["id"] == chunk_id:
                return row
    raise KeyError(f"Chunk id not found: {chunk_id}")


def load_audio_chunk(audio_path: Path, start: float, end: float, target_sr: int) -> tuple[torch.Tensor, float, float]:
    info = sf.info(str(audio_path))
    clip_end = info.duration if end <= 0.0 else min(end, info.duration)
    clip_start = max(0.0, start)
    start_frame = int(clip_start * info.samplerate)
    frame_count = max(1, int((clip_end - clip_start) * info.samplerate))
    audio, source_sr = sf.read(str(audio_path), start=start_frame, frames=frame_count, dtype="float32", always_2d=False)
    audio_tensor = torch.as_tensor(audio, dtype=torch.float32)
    if audio_tensor.ndim > 1:
        audio_tensor = audio_tensor.mean(dim=1)
    if int(source_sr) != target_sr:
        audio_tensor = torchaudio.functional.resample(audio_tensor, int(source_sr), target_sr)
    return audio_tensor, clip_start, clip_end


def make_log_mel(audio: torch.Tensor, sample_rate: int, n_fft: int, hop_length: int, n_mels: int) -> torch.Tensor:
    mel_transform = torchaudio.transforms.MelSpectrogram(
        sample_rate=sample_rate,
        n_fft=n_fft,
        hop_length=hop_length,
        n_mels=n_mels,
        center=True,
        power=2.0,
    )
    mel = mel_transform(audio)
    return torch.log(mel.clamp_min(1e-5))


def median_smooth_pitch(pitch: torch.Tensor, voiced: torch.Tensor, window: int) -> torch.Tensor:
    if window <= 1:
        return pitch
    radius = max(1, window // 2)
    output = pitch.clone()
    for idx in range(len(pitch)):
        if voiced[idx] <= 0:
            continue
        start = max(0, idx - radius)
        end = min(len(pitch), idx + radius + 1)
        values = pitch[start:end][voiced[start:end] > 0]
        if len(values) >= 3:
            output[idx] = values.median()
    return output


def pitch_logits_to_midi(
    pitch_logits: torch.Tensor,
    midi_min: int,
) -> torch.Tensor:
    pitch_bins = torch.argmax(pitch_logits, dim=0).to(torch.float32)
    return pitch_bins + float(midi_min)


def normalize_feature(values: np.ndarray) -> torch.Tensor:
    if values.size == 0:
        return torch.zeros(0, dtype=torch.float32)
    values = values.astype(np.float32)
    peak = float(np.max(values))
    if peak <= 1.0e-8:
        return torch.zeros(len(values), dtype=torch.float32)
    return torch.from_numpy(values / peak)


def build_audio_boundary_features(audio: torch.Tensor, sample_rate: int, hop_length: int) -> tuple[torch.Tensor, torch.Tensor]:
    audio_np = audio.detach().cpu().numpy().astype(np.float32)
    onset_env = librosa.onset.onset_strength(
        y=audio_np,
        sr=sample_rate,
        hop_length=hop_length,
        aggregate=np.median,
    )
    rms = librosa.feature.rms(y=audio_np, hop_length=hop_length, frame_length=hop_length * 4, center=True)[0]
    flux = np.maximum(0.0, np.diff(rms, prepend=rms[0]))
    return normalize_feature(onset_env), normalize_feature(flux)


def pad_or_trim_feature(feature: torch.Tensor, target_frames: int) -> torch.Tensor:
    if feature.numel() == target_frames:
        return feature
    if feature.numel() > target_frames:
        return feature[:target_frames]
    return torch.nn.functional.pad(feature, (0, target_frames - feature.numel()))


def decode_notes(
    voiced_probs: torch.Tensor,
    onset_probs: torch.Tensor,
    pitch_midi: torch.Tensor,
    hop_seconds: float,
    clip_start: float,
    voiced_threshold: float,
    onset_threshold: float,
    pitch_jump_semitones: float,
    min_note_seconds: float,
    median_window: int,
    audio_onset: torch.Tensor | None = None,
    audio_flux: torch.Tensor | None = None,
    ml_onset_weight: float = 0.55,
    audio_onset_weight: float = 0.30,
    audio_flux_weight: float = 0.15,
    min_same_pitch_split_seconds: float = 0.28,
) -> list[dict[str, float]]:
    voiced = (voiced_probs >= voiced_threshold).to(torch.int32)
    pitch = median_smooth_pitch(pitch_midi, voiced, median_window)
    min_note_frames = max(1, int(round(min_note_seconds / hop_seconds)))
    min_same_pitch_split_frames = max(1, int(round(min_same_pitch_split_seconds / hop_seconds)))

    notes: list[dict[str, float]] = []
    current_start: int | None = None
    pitch_values: list[float] = []
    voiced_values: list[float] = []
    onset_values: list[float] = []

    def commit(end_frame: int) -> None:
        nonlocal current_start, pitch_values, voiced_values, onset_values
        if current_start is None:
            return
        if end_frame - current_start < min_note_frames or not pitch_values:
            current_start = None
            pitch_values = []
            voiced_values = []
            onset_values = []
            return

        median_pitch = statistics.median(pitch_values)
        start_sec = clip_start + current_start * hop_seconds
        end_sec = clip_start + end_frame * hop_seconds
        notes.append(
            {
                "pitch": float(median_pitch),
                "start": float(start_sec),
                "duration": float(max(hop_seconds, end_sec - start_sec)),
                "confidence": float(sum(voiced_values) / max(1, len(voiced_values))),
                "onset_strength": float(max(onset_values) if onset_values else 0.0),
            }
        )
        current_start = None
        pitch_values = []
        voiced_values = []
        onset_values = []

    for frame in range(len(voiced)):
        is_voiced = bool(voiced[frame].item())
        frame_pitch = float(pitch[frame].item())
        onset_prob = float(onset_probs[frame].item())
        voiced_prob = float(voiced_probs[frame].item())

        if not is_voiced:
            if current_start is not None:
                commit(frame)
            continue

        audio_onset_value = float(audio_onset[frame].item()) if audio_onset is not None and frame < len(audio_onset) else 0.0
        audio_flux_value = float(audio_flux[frame].item()) if audio_flux is not None and frame < len(audio_flux) else 0.0
        boundary_score = (
            ml_onset_weight * onset_prob
            + audio_onset_weight * audio_onset_value
            + audio_flux_weight * audio_flux_value
        )

        starts_new = current_start is None
        if current_start is not None:
            prev_pitch = pitch_values[-1] if pitch_values else frame_pitch
            current_frames = frame - current_start
            pitch_delta = abs(frame_pitch - prev_pitch)
            if boundary_score >= onset_threshold and (
                pitch_delta >= 0.75 or current_frames >= min_same_pitch_split_frames
            ):
                starts_new = True
            elif pitch_delta >= pitch_jump_semitones:
                starts_new = True

        if starts_new:
            if current_start is not None:
                commit(frame)
            current_start = frame

        pitch_values.append(frame_pitch)
        voiced_values.append(voiced_prob)
        onset_values.append(boundary_score)

    if current_start is not None:
        commit(len(voiced))

    return cleanup_decoded_notes(notes, min_note_seconds)


def cleanup_decoded_notes(
    notes: list[dict[str, float]],
    min_note_seconds: float,
    max_merge_gap_seconds: float = 0.09,
    same_pitch_merge_semitones: float = 0.6,
) -> list[dict[str, float]]:
    if not notes:
        return []

    cleaned: list[dict[str, float]] = []
    for note in notes:
        if not cleaned:
            cleaned.append(note)
            continue

        prev = cleaned[-1]
        prev_end = float(prev["start"]) + float(prev["duration"])
        gap = float(note["start"]) - prev_end
        pitch_delta = abs(float(note["pitch"]) - float(prev["pitch"]))

        if gap <= max_merge_gap_seconds and pitch_delta <= same_pitch_merge_semitones:
            merged_end = max(prev_end, float(note["start"]) + float(note["duration"]))
            prev["duration"] = merged_end - float(prev["start"])
            prev["confidence"] = max(float(prev["confidence"]), float(note["confidence"]))
            prev["onset_strength"] = max(float(prev["onset_strength"]), float(note["onset_strength"]))
            prev["pitch"] = float((float(prev["pitch"]) + float(note["pitch"])) * 0.5)
            continue

        if (
            float(note["duration"]) < min_note_seconds * 1.15
            and gap <= max_merge_gap_seconds * 1.5
            and pitch_delta <= 1.5
        ):
            merged_end = max(prev_end, float(note["start"]) + float(note["duration"]))
            prev["duration"] = merged_end - float(prev["start"])
            prev["confidence"] = max(float(prev["confidence"]), float(note["confidence"]) * 0.9)
            prev["onset_strength"] = max(float(prev["onset_strength"]), float(note["onset_strength"]))
            continue

        cleaned.append(note)

    final_notes: list[dict[str, float]] = []
    for note in cleaned:
        if note["duration"] < min_note_seconds and final_notes:
            prev = final_notes[-1]
            prev_end = float(prev["start"]) + float(prev["duration"])
            gap = float(note["start"]) - prev_end
            pitch_delta = abs(float(note["pitch"]) - float(prev["pitch"]))
            if gap <= max_merge_gap_seconds * 1.5 and pitch_delta <= 1.5:
                merged_end = max(prev_end, float(note["start"]) + float(note["duration"]))
                prev["duration"] = merged_end - float(prev["start"])
                prev["confidence"] = max(float(prev["confidence"]), float(note["confidence"]))
                prev["onset_strength"] = max(float(prev["onset_strength"]), float(note["onset_strength"]))
                continue
        final_notes.append(note)

    return final_notes


def load_reference_notes(manifest_row: dict[str, Any]) -> list[dict[str, Any]]:
    label = json.loads(Path(manifest_row["label"]).read_text(encoding="utf-8"))
    start_idx, end_idx = manifest_row["notes_range"]
    return label["notes"][start_idx : end_idx + 1]


def summarize_alignment(predicted: list[dict[str, float]], reference: list[dict[str, Any]]) -> dict[str, Any]:
    ref_notes = [
        {
            "pitch": float(note["midi"]),
            "start": float(note["start"]),
            "duration": float(note["end"] - note["start"]),
        }
        for note in reference
    ]
    pairs = min(len(predicted), len(ref_notes))
    if pairs == 0:
        return {
            "predicted_count": len(predicted),
            "reference_count": len(ref_notes),
            "pitch_mae": None,
            "start_mae_seconds": None,
        }

    pitch_mae = sum(abs(predicted[i]["pitch"] - ref_notes[i]["pitch"]) for i in range(pairs)) / pairs
    start_mae = sum(abs(predicted[i]["start"] - ref_notes[i]["start"]) for i in range(pairs)) / pairs
    return {
        "predicted_count": len(predicted),
        "reference_count": len(ref_notes),
        "paired_notes": pairs,
        "pitch_mae": pitch_mae,
        "start_mae_seconds": start_mae,
    }


def main() -> int:
    args = parse_args()
    device = torch.device(args.device)
    model, train_args = load_checkpoint(args.checkpoint, device)

    if args.manifest_row:
        row = find_manifest_row(args.manifest, args.manifest_row)
        audio_path = Path(row["audio"])
        start = float(row["start"])
        end = float(row["end"])
        reference = load_reference_notes(row)
    else:
        if args.audio is None:
            raise ValueError("Either --audio or --manifest-row must be provided")
        row = None
        audio_path = args.audio
        start = args.start
        end = args.end
        reference = []

    sample_rate = int(train_args.get("sample_rate", 24000))
    hop_length = int(train_args.get("hop_length", 256))
    n_fft = int(train_args.get("n_fft", 1024))
    n_mels = int(train_args.get("n_mels", 80))
    midi_min = int(train_args.get("midi_min", 36))

    audio, clip_start, clip_end = load_audio_chunk(audio_path, start, end, sample_rate)
    log_mel = make_log_mel(audio, sample_rate, n_fft, hop_length, n_mels).unsqueeze(0).to(device)

    with torch.no_grad():
        predictions = model(log_mel)

    voiced_probs = torch.sigmoid(predictions["voiced_logits"]).squeeze(0).cpu()
    onset_probs = torch.sigmoid(predictions["onset_logits"]).squeeze(0).cpu()
    pitch_midi = pitch_logits_to_midi(predictions["pitch_logits"].squeeze(0).cpu(), midi_min)
    audio_onset, audio_flux = build_audio_boundary_features(audio, sample_rate, hop_length)
    audio_onset = pad_or_trim_feature(audio_onset, len(voiced_probs))
    audio_flux = pad_or_trim_feature(audio_flux, len(voiced_probs))

    notes = decode_notes(
        voiced_probs=voiced_probs,
        onset_probs=onset_probs,
        pitch_midi=pitch_midi,
        hop_seconds=hop_length / sample_rate,
        clip_start=clip_start,
        voiced_threshold=args.voiced_threshold,
        onset_threshold=args.hybrid_threshold if args.decode_mode == "hybrid" else args.onset_threshold,
        pitch_jump_semitones=args.pitch_jump_semitones,
        min_note_seconds=args.min_note_seconds,
        median_window=args.median_window,
        audio_onset=audio_onset if args.decode_mode == "hybrid" else None,
        audio_flux=audio_flux if args.decode_mode == "hybrid" else None,
        ml_onset_weight=args.ml_onset_weight,
        audio_onset_weight=args.audio_onset_weight,
        audio_flux_weight=args.audio_flux_weight,
        min_same_pitch_split_seconds=args.min_same_pitch_split_seconds,
    )

    payload: dict[str, Any] = {
        "audio": str(audio_path),
        "start": clip_start,
        "end": clip_end,
        "notes": notes,
        "frame_predictions": {
            "num_frames": int(len(voiced_probs)),
            "voiced_mean": float(voiced_probs.mean().item()),
            "onset_mean": float(onset_probs.mean().item()),
        },
        "decode_config": {
            "mode": args.decode_mode,
            "voiced_threshold": args.voiced_threshold,
            "onset_threshold": args.onset_threshold if args.decode_mode == "ml" else args.hybrid_threshold,
            "pitch_jump_semitones": args.pitch_jump_semitones,
        },
    }
    if row is not None:
        payload["manifest_row"] = row
        payload["reference_summary"] = summarize_alignment(notes, reference)
        payload["reference_notes"] = reference

    if args.output_json:
        args.output_json.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    else:
        print(json.dumps(payload, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
