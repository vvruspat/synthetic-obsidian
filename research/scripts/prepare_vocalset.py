#!/usr/bin/env python3
"""
VocalSet (and similar datasets) pre-processing pipeline.

For each audio file:
  1. Resample to 24 kHz mono
  2. Loudness-normalize to -23 LUFS
  3. Trim leading/trailing silence
  4. Pre-compute torchcrepe F0 + confidence, log-mel, A-weighted loudness
  5. Save as .pt tensor dict
  6. Write manifest.json

Usage:
    python prepare_vocalset.py \\
        --input_dir  /data/vocalset \\
        --output_dir /data/vocalset_processed \\
        --workers    8 \\
        --sample_rate 24000

Speaker ID is inferred from the parent directory name of each audio file.
"""

from __future__ import annotations

import argparse
import json
import os
import warnings
from multiprocessing import Pool, cpu_count
from pathlib import Path
from typing import Optional

import numpy as np
import torch
import torchaudio


# ---------------------------------------------------------------------------
# Global config (set once, read by worker processes)
# ---------------------------------------------------------------------------

_WORKER_CFG: dict = {}


def _init_worker(cfg: dict) -> None:
    """Initialise per-process config (avoids re-parsing in every worker call)."""
    global _WORKER_CFG
    _WORKER_CFG = cfg


# ---------------------------------------------------------------------------
# Loudness normalisation
# ---------------------------------------------------------------------------

def normalize_lufs(audio: np.ndarray, sr: int, target_lufs: float = -23.0) -> np.ndarray:
    """Normalize audio to target integrated loudness (LUFS).

    Uses pyloudnorm if available, else falls back to simple RMS normalization.
    """
    try:
        import pyloudnorm as pyln  # type: ignore
        meter = pyln.Meter(sr)
        with warnings.catch_warnings():
            warnings.simplefilter("ignore")
            current_lufs = meter.integrated_loudness(audio.astype(np.float64))
        if not np.isfinite(current_lufs) or current_lufs < -70.0:
            # Silent file — fall through to RMS
            raise ValueError("silence")
        gain_db = target_lufs - current_lufs
        gain_linear = 10.0 ** (gain_db / 20.0)
        audio = audio * gain_linear
    except Exception:
        # Simple RMS fallback: target RMS ≈ -23 LUFS ≈ -21 dB RMS
        rms = np.sqrt(np.mean(audio ** 2) + 1e-10)
        target_rms = 10.0 ** (target_lufs / 20.0)
        audio = audio * (target_rms / rms)
    return audio.astype(np.float32)


# ---------------------------------------------------------------------------
# Silence trimming
# ---------------------------------------------------------------------------

def trim_silence(
    audio: np.ndarray,
    sr: int,
    frame_len_ms: float = 20.0,
    energy_threshold_db: float = -50.0,
    min_speech_frames: int = 5,
) -> np.ndarray:
    """Trim leading and trailing silence using a simple energy threshold.

    Args:
        audio:                [T] float32 mono waveform.
        sr:                   Sample rate.
        frame_len_ms:         Analysis frame length in ms (default: 20 ms).
        energy_threshold_db:  Frames below this level are considered silent (default: -50 dB).
        min_speech_frames:    Minimum consecutive speech frames to start/end region.

    Returns:
        Trimmed audio [T'] float32.
    """
    frame_len = int(sr * frame_len_ms / 1000.0)
    n_frames = len(audio) // frame_len

    if n_frames == 0:
        return audio

    frames = audio[: n_frames * frame_len].reshape(n_frames, frame_len)
    rms_db = 20.0 * np.log10(np.sqrt(np.mean(frames ** 2, axis=1)) + 1e-10)
    is_speech = rms_db > energy_threshold_db

    # Find first and last speech frame
    speech_indices = np.where(is_speech)[0]
    if len(speech_indices) == 0:
        return audio  # Don't trim fully silent files

    start_frame = max(0, speech_indices[0] - 1)
    end_frame = min(n_frames, speech_indices[-1] + 2)

    start_sample = start_frame * frame_len
    end_sample = end_frame * frame_len

    return audio[start_sample:end_sample]


# ---------------------------------------------------------------------------
# Feature extraction
# ---------------------------------------------------------------------------

def compute_log_mel(
    audio_tensor: torch.Tensor,
    sr: int,
    n_fft: int,
    hop_length: int,
    n_mels: int,
) -> torch.Tensor:
    """Compute log-mel spectrogram. [T] → [n_mels, T_frames]."""
    mel_transform = torchaudio.transforms.MelSpectrogram(
        sample_rate=sr,
        n_fft=n_fft,
        hop_length=hop_length,
        n_mels=n_mels,
        f_min=20.0,
        f_max=sr / 2.0,
    )
    mel = mel_transform(audio_tensor.unsqueeze(0))  # [1, n_mels, T_frames]
    return torch.log(mel.squeeze(0).clamp(min=1e-5))  # [n_mels, T_frames]


def compute_a_weighted_loudness(
    audio_tensor: torch.Tensor,
    sr: int,
    n_fft: int,
    hop_length: int,
) -> torch.Tensor:
    """Compute per-frame A-weighted loudness in dB. [T] → [T_frames]."""
    freqs = torch.linspace(0, sr / 2.0, n_fft // 2 + 1)
    f2 = freqs ** 2
    num = (12194.0 ** 2) * (f2 ** 2)
    denom = (
        (f2 + 20.6 ** 2)
        * torch.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2))
        * (f2 + 12194.0 ** 2)
    )
    import math
    ra_1khz = (12194.0 ** 2) * (1000.0 ** 4) / (
        (1000.0 ** 2 + 20.6 ** 2)
        * math.sqrt((1000.0 ** 2 + 107.7 ** 2) * (1000.0 ** 2 + 737.9 ** 2))
        * (1000.0 ** 2 + 12194.0 ** 2)
    )
    a_weights = torch.where(freqs > 0, num / (denom + 1e-12), torch.zeros_like(freqs))
    a_weights = a_weights / (ra_1khz + 1e-12)

    window = torch.hann_window(n_fft)
    stft = torch.stft(
        audio_tensor,
        n_fft=n_fft,
        hop_length=hop_length,
        win_length=n_fft,
        window=window,
        return_complex=True,
    )  # [F, T_frames]
    power = stft.abs() ** 2
    a_weighted = (power * a_weights.unsqueeze(-1)).mean(dim=0)  # [T_frames]
    return 10.0 * torch.log10(a_weighted.clamp(min=1e-10))  # [T_frames]


def compute_f0(
    audio_np: np.ndarray,
    sr: int,
    hop_length: int,
    fmin: float = 50.0,
    fmax: float = 1000.0,
    model: str = "full",
    confidence_threshold: float = 0.4,
    device: str = "cpu",
) -> tuple[np.ndarray, np.ndarray]:
    """Estimate F0 using torchcrepe. Returns (f0_hz, voiced) as numpy arrays."""
    try:
        import torchcrepe  # type: ignore

        wav = torch.from_numpy(audio_np).unsqueeze(0)
        f0, conf = torchcrepe.predict(
            wav,
            sr,
            hop_length,
            fmin,
            fmax,
            model=model,
            return_periodicity=True,
            device=torch.device(device),
            batch_size=512,
        )
        f0_np = f0.squeeze(0).numpy()
        voiced_np = (conf.squeeze(0).numpy() >= confidence_threshold)
        f0_np = f0_np * voiced_np  # zero out unvoiced
        return f0_np.astype(np.float32), voiced_np
    except ImportError:
        # Fallback: zero F0
        n_frames = len(audio_np) // hop_length
        return np.zeros(n_frames, dtype=np.float32), np.zeros(n_frames, dtype=bool)


# ---------------------------------------------------------------------------
# Worker function
# ---------------------------------------------------------------------------

def process_file(args_tuple: tuple) -> Optional[dict]:
    """Process a single audio file. Returns manifest entry or None on failure."""
    file_path, output_dir = args_tuple
    cfg = _WORKER_CFG
    sr = cfg["sample_rate"]
    hop = cfg["hop_length"]
    n_fft = cfg["n_fft"]
    n_mels = cfg["n_mels"]
    target_lufs = cfg["target_lufs"]
    crepe_model = cfg["crepe_model"]
    device = cfg["device"]

    file_path = Path(file_path)
    output_dir = Path(output_dir)
    speaker_id = file_path.parent.name

    out_subdir = output_dir / speaker_id
    out_subdir.mkdir(parents=True, exist_ok=True)
    out_path = out_subdir / (file_path.stem + ".pt")

    if out_path.exists():
        # Load existing manifest entry from saved tensor
        try:
            data = torch.load(str(out_path), weights_only=False)
            duration_s = len(data["audio"]) / data["sample_rate"]
            return {"path": str(out_path), "speaker_id": speaker_id, "duration_s": duration_s}
        except Exception:
            pass

    try:
        # 1. Load and resample
        audio_tensor, orig_sr = torchaudio.load(str(file_path))
        audio_tensor = audio_tensor.mean(dim=0)  # stereo → mono [T]

        if orig_sr != sr:
            audio_tensor = torchaudio.functional.resample(audio_tensor, orig_sr, sr)

        audio_np = audio_tensor.numpy()

        # 2. Loudness normalisation
        audio_np = normalize_lufs(audio_np, sr, target_lufs=target_lufs)

        # 3. Trim silence
        audio_np = trim_silence(audio_np, sr)

        if len(audio_np) < sr * 0.1:  # skip files shorter than 100ms after trimming
            return None

        audio_tensor = torch.from_numpy(audio_np)

        # 4. Feature extraction
        log_mel = compute_log_mel(audio_tensor, sr, n_fft, hop, n_mels)      # [n_mels, T_frames]
        loudness_db = compute_a_weighted_loudness(audio_tensor, sr, n_fft, hop)  # [T_frames]
        f0_hz, voiced = compute_f0(audio_np, sr, hop, model=crepe_model, device=device)
        f0_tensor = torch.from_numpy(f0_hz)
        voiced_tensor = torch.from_numpy(voiced)

        # 5. Save .pt
        torch.save(
            {
                "audio": audio_tensor,
                "f0_hz": f0_tensor,
                "voiced": voiced_tensor,
                "log_mel": log_mel,
                "loudness_db": loudness_db,
                "sample_rate": sr,
            },
            str(out_path),
        )

        duration_s = len(audio_np) / sr
        return {"path": str(out_path), "speaker_id": speaker_id, "duration_s": duration_s}

    except Exception as e:
        print(f"  ERROR processing {file_path}: {e}")
        return None


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(description="Prepare vocal dataset for NVT training.")
    parser.add_argument("--input_dir", required=True, help="Root directory with audio files.")
    parser.add_argument("--output_dir", required=True, help="Output directory for .pt files.")
    parser.add_argument("--sample_rate", type=int, default=24000, help="Target sample rate.")
    parser.add_argument("--hop_length", type=int, default=256, help="Analysis hop length.")
    parser.add_argument("--n_fft", type=int, default=1024, help="FFT size.")
    parser.add_argument("--n_mels", type=int, default=80, help="Number of mel bins.")
    parser.add_argument("--target_lufs", type=float, default=-23.0, help="Target LUFS.")
    parser.add_argument(
        "--crepe_model",
        default="full",
        choices=["full", "tiny"],
        help="torchcrepe model variant (default: full).",
    )
    parser.add_argument("--workers", type=int, default=max(1, cpu_count() // 2), help="Worker count.")
    parser.add_argument(
        "--device",
        default="cpu",
        help="Device for torchcrepe inference (default: cpu). Use 'cuda' if available.",
    )
    parser.add_argument("--extensions", nargs="+", default=[".wav", ".flac"], help="Audio extensions.")
    args = parser.parse_args()

    input_dir = Path(args.input_dir)
    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Collect audio files
    audio_files = []
    for ext in args.extensions:
        audio_files.extend(input_dir.rglob(f"*{ext}"))
    audio_files = sorted(audio_files)

    print(f"Found {len(audio_files)} audio files in {input_dir}")
    print(f"Output dir: {output_dir}")
    print(f"Workers: {args.workers}  |  crepe: {args.crepe_model}  |  device: {args.device}")

    cfg = {
        "sample_rate": args.sample_rate,
        "hop_length": args.hop_length,
        "n_fft": args.n_fft,
        "n_mels": args.n_mels,
        "target_lufs": args.target_lufs,
        "crepe_model": args.crepe_model,
        "device": args.device,
    }

    worker_args = [(str(f), str(output_dir)) for f in audio_files]

    # Note: torchcrepe with CUDA is not safe with multiprocessing — use 1 worker for GPU
    n_workers = 1 if args.device != "cpu" else args.workers

    manifest = []
    try:
        with Pool(processes=n_workers, initializer=_init_worker, initargs=(cfg,)) as pool:
            for i, result in enumerate(pool.imap_unordered(process_file, worker_args), 1):
                if result is not None:
                    manifest.append(result)
                if i % 100 == 0 or i == len(worker_args):
                    print(f"  Processed {i}/{len(worker_args)}  ({len(manifest)} OK)")
    except KeyboardInterrupt:
        print("\nInterrupted. Saving partial manifest.")

    # Write manifest
    manifest_path = output_dir / "manifest.json"
    with open(manifest_path, "w") as f:
        json.dump(manifest, f, indent=2)

    total_duration = sum(e["duration_s"] for e in manifest)
    speakers = set(e["speaker_id"] for e in manifest)
    print(f"\nDone. {len(manifest)} files  |  {total_duration/3600:.2f}h  |  {len(speakers)} speakers")
    print(f"Manifest → {manifest_path}")


if __name__ == "__main__":
    main()
