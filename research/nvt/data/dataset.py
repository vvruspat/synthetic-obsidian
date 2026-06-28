"""
NVT Dataset — loads pre-processed .pt files from prepare_vocalset.py output.

Each .pt file contains:
    audio       [T]             float32 mono waveform at 24 kHz
    f0_hz       [T_frames]      float32 fundamental frequency (0 = unvoiced)
    voiced      [T_frames]      bool
    log_mel     [n_mels, T_mel] float32
    loudness_db [T_frames]      float32 A-weighted loudness in dB
    sample_rate int

NVTDataset slices fixed-length segments (segment_samples long) with random
offset, so every __getitem__ call returns a different crop of the file.
"""

from __future__ import annotations

import json
import random
from pathlib import Path
from typing import Dict, List, Optional

import torch
import torchaudio
from torch.utils.data import Dataset


class NVTDataset(Dataset):
    """Dataset that reads pre-processed .pt tensor files.

    Args:
        manifest_path:   Path to manifest.json produced by prepare_vocalset.py.
        segment_seconds: Duration of each training segment in seconds.
        sample_rate:     Expected sample rate (files not matching are skipped).
        hop_length:      Analysis hop length (used to align audio / frame tensors).
        n_mels:          Expected number of mel bins (sanity check).
        min_duration_s:  Skip files shorter than this (default: 1.0 s).
        augment:         Apply light data augmentation (gain jitter) if True.
    """

    def __init__(
        self,
        manifest_path: str | Path,
        segment_seconds: float = 4.0,
        sample_rate: int = 24000,
        hop_length: int = 256,
        n_mels: int = 80,
        min_duration_s: float = 1.0,
        augment: bool = True,
    ) -> None:
        self.segment_samples = int(sample_rate * segment_seconds)
        self.segment_frames = self.segment_samples // hop_length
        self.hop_length = hop_length
        self.sample_rate = sample_rate
        self.n_mels = n_mels
        self.augment = augment

        manifest_path = Path(manifest_path)
        if not manifest_path.exists():
            raise FileNotFoundError(f"Manifest not found: {manifest_path}")

        with open(manifest_path) as f:
            manifest = json.load(f)

        self.entries: List[Path] = []
        for entry in manifest:
            p = Path(entry["path"])
            dur = entry.get("duration_s", 0.0)
            if p.exists() and dur >= min_duration_s:
                self.entries.append(p)

        if not self.entries:
            raise ValueError(f"No valid entries found in manifest: {manifest_path}")

        print(f"[NVTDataset] {len(self.entries)} files from {manifest_path}")

    def __len__(self) -> int:
        # Each file can yield multiple non-overlapping segments; use 5× file count
        # so every file gets visited multiple times per epoch.
        return len(self.entries) * 5

    def __getitem__(self, idx: int) -> Dict[str, torch.Tensor]:
        # Pick a random file (not index-tied so every epoch sees different crops)
        path = random.choice(self.entries)

        try:
            data = torch.load(str(path), weights_only=True)
        except Exception:
            # weights_only=True may fail on older saves — fall back
            data = torch.load(str(path), weights_only=False)

        audio: torch.Tensor = data["audio"]           # [T]
        f0_hz: torch.Tensor = data["f0_hz"]           # [T_frames]
        voiced: torch.Tensor = data["voiced"]         # [T_frames] bool
        loudness_db: torch.Tensor = data["loudness_db"]  # [T_frames]
        log_mel: torch.Tensor = data["log_mel"]       # [n_mels, T_mel]

        T_audio = audio.shape[0]
        T_frames = f0_hz.shape[0]

        # ── Random crop ────────────────────────────────────────────────────────
        seg_samples = min(self.segment_samples, T_audio)
        seg_frames  = seg_samples // self.hop_length

        if T_audio > seg_samples:
            start_sample = random.randint(0, T_audio - seg_samples)
        else:
            start_sample = 0

        start_frame = start_sample // self.hop_length
        end_sample  = start_sample + seg_samples
        end_frame   = min(start_frame + seg_frames, T_frames)

        audio_seg     = audio[start_sample:end_sample]
        f0_seg        = f0_hz[start_frame:end_frame]
        voiced_seg    = voiced[start_frame:end_frame]
        loudness_seg  = loudness_db[start_frame:end_frame]
        mel_seg       = log_mel[:, start_frame:end_frame]

        # Always pad/trim to FIXED segment length so DataLoader can stack tensors.
        # seg_frames is the frame count for the FIXED segment_samples length.
        fixed_samples = self.segment_samples
        fixed_frames  = self.segment_frames

        def _pad1d(x: torch.Tensor, target: int) -> torch.Tensor:
            if x.shape[0] < target:
                return torch.nn.functional.pad(x, (0, target - x.shape[0]))
            return x[:target]

        def _pad2d(x: torch.Tensor, target: int) -> torch.Tensor:
            if x.shape[1] < target:
                return torch.nn.functional.pad(x, (0, target - x.shape[1]))
            return x[:, :target]

        audio_seg    = _pad1d(audio_seg, fixed_samples)
        f0_seg       = _pad1d(f0_seg, fixed_frames)
        voiced_seg   = _pad1d(voiced_seg.float(), fixed_frames).bool()
        loudness_seg = _pad1d(loudness_seg, fixed_frames)
        mel_seg      = _pad2d(mel_seg, fixed_frames)

        # ── Light augmentation: random gain ±6 dB ─────────────────────────────
        if self.augment:
            gain_db = random.uniform(-6.0, 6.0)
            gain = 10.0 ** (gain_db / 20.0)
            audio_seg = audio_seg * gain
            loudness_seg = loudness_seg + gain_db

        return {
            "audio":        audio_seg,        # [T_seg]
            "f0_hz":        f0_seg,           # [T_frames]
            "voiced":       voiced_seg,       # [T_frames] bool
            "loudness_db":  loudness_seg,     # [T_frames]
            "log_mel":      mel_seg,          # [n_mels, T_frames]
            "sample_rate":  self.sample_rate,
        }
