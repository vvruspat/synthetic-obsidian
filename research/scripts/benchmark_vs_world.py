#!/usr/bin/env python3
"""
Benchmark: WORLD+Vocos vs DDSP Neural Vocal Tuner pitch shifting.

Usage:
    python benchmark_vs_world.py --input vocals.wav --semitones 3 --output_dir ./bench_out
    python benchmark_vs_world.py --input vocals.wav --semitones -2 --metrics --output_dir ./bench_out
    python benchmark_vs_world.py --input vocals.wav --checkpoint path/to/nvt.pt --semitones 3
"""

from __future__ import annotations

import argparse
import os
import time
import json
from pathlib import Path

import numpy as np
import soundfile as sf


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def load_wav_mono(path: str, target_sr: int = 24000) -> tuple[np.ndarray, int]:
    """Load WAV/FLAC file, convert to mono float32, resample if needed."""
    import soundfile as sf

    audio, sr = sf.read(path, dtype="float32", always_2d=False)
    if audio.ndim == 2:
        audio = audio.mean(axis=1)

    if sr != target_sr:
        try:
            import librosa  # type: ignore
            audio = librosa.resample(audio, orig_sr=sr, target_sr=target_sr)
        except ImportError:
            import scipy.signal as ss
            n_samples = int(len(audio) * target_sr / sr)
            audio = ss.resample(audio, n_samples)
        sr = target_sr

    return audio, sr


def semitones_to_ratio(semitones: float) -> float:
    """Convert semitone shift to frequency ratio."""
    return 2.0 ** (semitones / 12.0)


def shift_f0(f0: np.ndarray, ratio: float) -> np.ndarray:
    """Multiply voiced F0 frames by ratio (leave 0-frames unchanged)."""
    f0_shifted = f0.copy()
    voiced = f0 > 0
    f0_shifted[voiced] *= ratio
    return f0_shifted


def log_spectral_distance(ref: np.ndarray, deg: np.ndarray, sr: int, n_fft: int = 1024) -> float:
    """Compute Log Spectral Distance (LSD) in dB between two waveforms."""
    import scipy.signal as ss

    min_len = min(len(ref), len(deg))
    ref = ref[:min_len]
    deg = deg[:min_len]

    hop = n_fft // 4
    _, _, ref_stft = ss.stft(ref, fs=sr, nperseg=n_fft, noverlap=n_fft - hop)
    _, _, deg_stft = ss.stft(deg, fs=sr, nperseg=n_fft, noverlap=n_fft - hop)

    ref_log = np.log(np.abs(ref_stft) ** 2 + 1e-10)
    deg_log = np.log(np.abs(deg_stft) ** 2 + 1e-10)

    lsd = np.sqrt(np.mean((ref_log - deg_log) ** 2))
    return float(lsd)


# ---------------------------------------------------------------------------
# WORLD + Vocos pipeline
# ---------------------------------------------------------------------------

def pitch_shift_world_vocos(
    audio: np.ndarray,
    sr: int,
    semitones: float,
    vocos_checkpoint: str = "charactr/vocos-mel-24khz",
) -> tuple[np.ndarray, float]:
    """Apply pitch shift using pyworld synthesis + Vocos vocoder.

    Returns:
        output audio [T] float32, elapsed time in seconds.
    """
    import pyworld  # type: ignore

    t0 = time.perf_counter()

    audio_d = audio.astype(np.float64)
    frame_period = 5.0  # ms

    # Analysis
    f0, sp, ap = pyworld.wav2world(audio_d, sr, frame_period=frame_period)

    # Pitch shift: multiply voiced F0 by ratio
    ratio = semitones_to_ratio(semitones)
    f0_shifted = shift_f0(f0, ratio)

    # Synthesis
    out_d = pyworld.synthesize(f0_shifted, sp, ap, sr, frame_period=frame_period)
    out = out_d.astype(np.float32)

    # Vocos post-processing
    try:
        import torch  # type: ignore
        import torchaudio  # type: ignore
        from vocos import Vocos  # type: ignore

        vocos = Vocos.from_pretrained(vocos_checkpoint)
        vocos.eval()

        wav_tensor = torch.from_numpy(out).unsqueeze(0)  # [1, T]

        # Compute mel
        mel_transform = torchaudio.transforms.MelSpectrogram(
            sample_rate=sr,
            n_fft=1024,
            hop_length=256,
            n_mels=80,
            f_min=20.0,
        )
        with torch.no_grad():
            mel = mel_transform(wav_tensor)
            log_mel = torch.log(mel.clamp(min=1e-5))
            out_tensor = vocos.decode(log_mel)

        out = out_tensor.squeeze(0).numpy()
    except Exception as e:
        print(f"  [Vocos] Skipped ({e.__class__.__name__}: {e}), using raw WORLD output.")

    elapsed = time.perf_counter() - t0
    return out, elapsed


# ---------------------------------------------------------------------------
# DDSP NVT pipeline
# ---------------------------------------------------------------------------

def pitch_shift_ddsp(
    audio: np.ndarray,
    sr: int,
    semitones: float,
    checkpoint: str,
) -> tuple[np.ndarray, float]:
    """Apply pitch shift using NeuralVocalTuner.

    Returns:
        output audio [T] float32, elapsed time in seconds.
    """
    import torch  # type: ignore
    import sys

    # Ensure research/ is on path
    research_dir = Path(__file__).resolve().parent.parent
    if str(research_dir) not in sys.path:
        sys.path.insert(0, str(research_dir))

    from nvt.models.full_model import NeuralVocalTuner  # type: ignore

    t0 = time.perf_counter()

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")

    state = torch.load(checkpoint, map_location=device)
    ckpt_cfg = state.get("config", {}) if isinstance(state, dict) else {}
    model = NeuralVocalTuner(
        sample_rate=sr,
        n_harmonics=int(ckpt_cfg.get("n_harmonics", 100)),
        timbre_dim=int(ckpt_cfg.get("timbre_dim", 64)),
        noise_dim=int(ckpt_cfg.get("noise_dim", 32)),
        noise_filter_len=int(ckpt_cfg.get("noise_filter_len", 128)),
    )
    model_state = state.get("model", state) if isinstance(state, dict) else state
    current_state = model.state_dict()
    compatible_state = {
        key: value
        for key, value in model_state.items()
        if key in current_state and current_state[key].shape == value.shape
    }
    model.load_state_dict(compatible_state, strict=False)
    model.eval().to(device)

    wav_tensor = torch.from_numpy(audio).unsqueeze(0).to(device)  # [1, T]

    # Encode to get F0
    with torch.no_grad():
        latents = model.encode(wav_tensor)
        f0_hz = latents["f0_hz"]  # [1, T_frames]

    # Shift F0
    ratio = semitones_to_ratio(semitones)
    f0_shifted = f0_hz.clone()
    voiced = f0_hz > 0
    f0_shifted[voiced] = f0_hz[voiced] * ratio

    # Decode with shifted F0
    with torch.no_grad():
        out_tensor = model.decode(
            f0_hz=f0_shifted,
            loudness=latents["loudness"],
            z_timbre=latents["z_timbre"],
            z_noise=latents["z_noise"],
        )

    out = out_tensor.squeeze(0).cpu().numpy()
    elapsed = time.perf_counter() - t0
    return out, elapsed


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> None:
    parser = argparse.ArgumentParser(
        description="Benchmark WORLD+Vocos vs DDSP NeuralVocalTuner pitch shifting."
    )
    parser.add_argument("--input", required=True, help="Input WAV/FLAC file path.")
    parser.add_argument("--semitones", type=float, default=3.0, help="Semitone shift (default: +3).")
    parser.add_argument("--output_dir", default="./bench_output", help="Output directory.")
    parser.add_argument(
        "--checkpoint",
        default=None,
        help="Path to NeuralVocalTuner checkpoint (.pt). If omitted, DDSP column is skipped.",
    )
    parser.add_argument(
        "--metrics",
        action="store_true",
        help="Compute LSD (Log Spectral Distance) between original and each output.",
    )
    parser.add_argument("--sample_rate", type=int, default=24000, help="Target sample rate (default: 24000).")
    parser.add_argument(
        "--vocos_checkpoint",
        default="charactr/vocos-mel-24khz",
        help="Vocos HuggingFace checkpoint ID.",
    )
    args = parser.parse_args()

    output_dir = Path(args.output_dir)
    output_dir.mkdir(parents=True, exist_ok=True)

    # Load input
    print(f"Loading: {args.input}")
    audio, sr = load_wav_mono(args.input, target_sr=args.sample_rate)
    print(f"  duration: {len(audio)/sr:.2f}s  sr: {sr} Hz")

    # Save original
    orig_path = output_dir / "original.wav"
    sf.write(str(orig_path), audio, sr)
    print(f"  Saved original → {orig_path}")

    results: dict = {"input": args.input, "semitones": args.semitones, "methods": {}}

    # ----- WORLD + Vocos -----
    print(f"\n[1/2] WORLD+Vocos  ({args.semitones:+.1f} semitones)")
    try:
        world_out, world_time = pitch_shift_world_vocos(
            audio, sr, args.semitones, vocos_checkpoint=args.vocos_checkpoint
        )
        world_path = output_dir / "world_vocos.wav"
        sf.write(str(world_path), world_out, sr)
        print(f"  Time: {world_time:.3f}s  →  {world_path}")
        results["methods"]["world_vocos"] = {"time_s": world_time, "output": str(world_path)}

        if args.metrics:
            lsd = log_spectral_distance(audio, world_out, sr)
            print(f"  LSD vs original: {lsd:.4f}")
            results["methods"]["world_vocos"]["lsd"] = lsd
    except Exception as e:
        print(f"  FAILED: {e}")
        results["methods"]["world_vocos"] = {"error": str(e)}

    # ----- DDSP NVT -----
    print(f"\n[2/2] DDSP NeuralVocalTuner  ({args.semitones:+.1f} semitones)")
    if args.checkpoint is None:
        print("  Skipped (no --checkpoint provided).")
        results["methods"]["ddsp_nvt"] = {"skipped": True, "reason": "no checkpoint"}
    else:
        try:
            ddsp_out, ddsp_time = pitch_shift_ddsp(audio, sr, args.semitones, args.checkpoint)
            ddsp_path = output_dir / "ddsp_nvt.wav"
            sf.write(str(ddsp_path), ddsp_out, sr)
            print(f"  Time: {ddsp_time:.3f}s  →  {ddsp_path}")
            results["methods"]["ddsp_nvt"] = {"time_s": ddsp_time, "output": str(ddsp_path)}

            if args.metrics:
                lsd = log_spectral_distance(audio, ddsp_out, sr)
                print(f"  LSD vs original: {lsd:.4f}")
                results["methods"]["ddsp_nvt"]["lsd"] = lsd
        except Exception as e:
            print(f"  FAILED: {e}")
            results["methods"]["ddsp_nvt"] = {"error": str(e)}

    # ----- Summary -----
    print("\n===== SUMMARY =====")
    print(f"Input:     {args.input}  ({len(audio)/sr:.2f}s)")
    print(f"Shift:     {args.semitones:+.1f} semitones")
    for method, info in results["methods"].items():
        if "time_s" in info:
            rtf = info["time_s"] / (len(audio) / sr)
            metrics_str = f"  LSD={info['lsd']:.4f}" if args.metrics and "lsd" in info else ""
            print(f"  {method:<20}  {info['time_s']:.3f}s  (RTF={rtf:.2f}){metrics_str}")
        elif "skipped" in info:
            print(f"  {method:<20}  SKIPPED")
        else:
            print(f"  {method:<20}  ERROR: {info.get('error', '?')}")

    # Save JSON results
    results_path = output_dir / "results.json"
    with open(results_path, "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nResults JSON → {results_path}")


if __name__ == "__main__":
    main()
