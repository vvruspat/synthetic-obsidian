#!/usr/bin/env python3
"""Post-render polish for SVC/RVC wav outputs.

The SVC model can produce excellent tone while still leaving isolated waveform
spikes or samples pinned at full scale. This script keeps the render intact and
only applies conservative offline cleanup:

- remove DC offset
- repair very short clipped plateaus
- smooth isolated single-sample jumps
- add a little peak headroom
"""

from __future__ import annotations

import argparse
import math
import shutil
from pathlib import Path

import torch
import soundfile as sf


def _fade_edges(audio: torch.Tensor, sample_rate: int, fade_ms: float = 5.0) -> torch.Tensor:
    n = min(audio.shape[-1] // 2, max(1, int(sample_rate * fade_ms / 1000.0)))
    if n <= 1:
        return audio
    fade_in = torch.linspace(0.0, 1.0, n, dtype=audio.dtype, device=audio.device)
    fade_out = torch.flip(fade_in, dims=[0])
    audio = audio.clone()
    audio[..., :n] *= fade_in
    audio[..., -n:] *= fade_out
    return audio


def _repair_short_clips(
    channel: torch.Tensor,
    clip_threshold: float = 0.997,
    max_run: int = 96,
) -> torch.Tensor:
    y = channel.clone()
    clipped = torch.nonzero(y.abs() >= clip_threshold, as_tuple=False).flatten()
    if clipped.numel() == 0:
        return y

    runs: list[tuple[int, int]] = []
    start = int(clipped[0].item())
    prev = start
    for idx_t in clipped[1:]:
        idx = int(idx_t.item())
        if idx == prev + 1:
            prev = idx
            continue
        runs.append((start, prev))
        start = prev = idx
    runs.append((start, prev))

    for start, end in runs:
        run_len = end - start + 1
        left = start - 1
        right = end + 1
        if run_len > max_run or left < 0 or right >= y.numel():
            continue

        n = right - left + 1
        t = torch.linspace(0.0, 1.0, n, dtype=y.dtype, device=y.device)
        # Smoothstep interpolation avoids adding a sharp slope at the repair
        # boundaries, which would be another click.
        smooth = t * t * (3.0 - 2.0 * t)
        y[left : right + 1] = y[left] + (y[right] - y[left]) * smooth

    return y


def _smooth_isolated_jumps(
    channel: torch.Tensor,
    jump_threshold: float = 0.32,
    window: int = 32,
    max_repairs: int = 256,
) -> torch.Tensor:
    y = channel.clone()
    jumps = torch.nonzero((y[1:] - y[:-1]).abs() > jump_threshold, as_tuple=False).flatten()
    if jumps.numel() == 0:
        return y

    repaired = 0
    last_end = -1
    for jump_t in jumps:
        if repaired >= max_repairs:
            break
        center = int(jump_t.item()) + 1
        start = max(0, center - window)
        end = min(y.numel() - 1, center + window)
        if start <= last_end or end - start < 8:
            continue

        before = y[max(0, start - window) : start]
        after = y[end + 1 : min(y.numel(), end + 1 + window)]
        if before.numel() == 0 or after.numel() == 0:
            continue

        local_scale = torch.cat([before, after]).abs().median().item()
        # Avoid flattening legitimate loud consonant transients.
        if local_scale > 0.55:
            continue

        n = end - start + 1
        t = torch.linspace(0.0, 1.0, n, dtype=y.dtype, device=y.device)
        smooth = t * t * (3.0 - 2.0 * t)
        y[start : end + 1] = y[start] + (y[end] - y[start]) * smooth
        last_end = end
        repaired += 1

    return y


def polish_audio(
    audio: torch.Tensor,
    sample_rate: int,
    target_peak_db: float = -1.5,
    mode: str = "repair",
) -> torch.Tensor:
    y = audio.float().clone()
    y = y - y.mean(dim=-1, keepdim=True)

    if mode == "repair":
        channels = []
        for channel in y:
            channel = _repair_short_clips(channel)
            channel = _smooth_isolated_jumps(channel)
            channels.append(channel)
        y = torch.stack(channels, dim=0)
    elif mode != "headroom":
        raise ValueError(f"Unknown polish mode: {mode}")

    y = _fade_edges(y, sample_rate)

    peak = y.abs().max().clamp_min(1e-8)
    target_peak = 10.0 ** (target_peak_db / 20.0)
    if peak > target_peak:
        y = y * (target_peak / peak)

    return y


def polish_tree(input_dir: Path, output_dir: Path, target_peak_db: float, mode: str) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    for wav_path in sorted(input_dir.rglob("*.wav")):
        rel = wav_path.relative_to(input_dir)
        out_path = output_dir / rel
        out_path.parent.mkdir(parents=True, exist_ok=True)

        audio_np, sample_rate = sf.read(wav_path, always_2d=True, dtype="float32")
        audio = torch.from_numpy(audio_np.T.copy())
        polished = polish_audio(audio, sample_rate, target_peak_db=target_peak_db, mode=mode)
        sf.write(out_path, polished.transpose(0, 1).cpu().numpy(), sample_rate, subtype="FLOAT")

    manifest = output_dir / "POLISHING.txt"
    manifest.write_text(
        "\n".join(
            [
                "Post-render SVC polish",
                f"source: {input_dir}",
                f"mode: {mode}",
                f"target peak: {target_peak_db:.1f} dBFS",
                "repair mode: DC removal, short clip repair, isolated jump smoothing, edge fades, peak headroom",
                "headroom mode: DC removal, edge fades, peak headroom only",
                "",
            ]
        ),
        encoding="utf-8",
    )


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--target-peak-db", type=float, default=-1.5)
    parser.add_argument("--mode", choices=("repair", "headroom"), default="repair")
    args = parser.parse_args()

    polish_tree(
        input_dir=args.input_dir.resolve(),
        output_dir=args.output_dir.resolve(),
        target_peak_db=args.target_peak_db,
        mode=args.mode,
    )


if __name__ == "__main__":
    main()
