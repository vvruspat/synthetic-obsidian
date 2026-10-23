"""Backward-compatible experimental input: two time scales and noise cues."""
from __future__ import annotations

import numpy as np
import torch
from torch import nn

from research.gtsinger_alignment.train_baseline import ResidualDilatedBlock
from .model import NoteBoundaryNet, SAMPLE_RATE, HOP, MELS, features, SCHEMA as LEGACY_SCHEMA

SCHEMA = "synthetic-obsidian-note-boundary-tcn-v2"
CONTRACT = "strongest-linear-multiscale-noise-clipnorm-v2"
LONG_FFT = 2048  # 128 ms alongside the legacy 32 ms branch.
DILATIONS = (1, 2, 4, 8, 16, 16)
FEATURES = 2 * MELS + 3


def long_filters():
    import librosa
    return librosa.filters.mel(sr=SAMPLE_RATE, n_fft=LONG_FFT, n_mels=MELS,
                              fmin=40.0, fmax=7600.0).astype(np.float32)


def multiscale_features(audio, sample_rate):
    short = features(audio, sample_rate)
    if audio.ndim == 1:
        audio = audio[:, None]
    mono = audio[:, np.argmax(np.square(audio, dtype=np.float64).sum(axis=0))]
    count = int(np.ceil(len(mono) * SAMPLE_RATE / sample_rate))
    positions = np.arange(count, dtype=np.float64) * sample_rate / SAMPLE_RATE
    mono = np.interp(positions, np.arange(len(mono)), mono).astype(np.float32)
    windows = np.lib.stride_tricks.sliding_window_view(
        np.pad(mono, (LONG_FFT // 2, LONG_FFT // 2)), LONG_FFT)[::HOP]
    hann = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(LONG_FFT) / LONG_FFT)).astype(np.float32)
    power = np.abs(np.fft.rfft(windows * hann)).astype(np.float64) ** 2
    energy = power.sum(axis=1)
    flatness = np.exp(np.log(np.maximum(power, 1e-10)).mean(axis=1)) / np.maximum(power.mean(axis=1), 1e-10)
    high_ratio = power[:, LONG_FFT // 4:].sum(axis=1) / np.maximum(energy, 1e-10)
    level = np.log10(np.maximum(energy, 1e-10))
    level = (level - level.mean()) / max(float(level.std()), 1e-5)
    db = (10 * np.log10(np.maximum(long_filters().astype(np.float64) @ power.T, 1e-10))).astype(np.float32)
    db = np.maximum(db, db.max() - 80.0)
    long = (db - db.mean(axis=1, keepdims=True, dtype=np.float64)) / np.maximum(
        db.std(axis=1, keepdims=True, dtype=np.float64), 1e-5)
    return np.concatenate([short, long, np.stack([flatness, high_ratio, level])]).astype(np.float32)


class MultiScaleNoteBoundaryNet(nn.Module):
    def __init__(self, input_features=FEATURES):
        super().__init__()
        self.input_projection = nn.Sequential(nn.Conv1d(input_features, 64, 1), nn.ReLU())
        self.temporal_blocks = nn.Sequential(*(ResidualDilatedBlock(64, 3, d) for d in DILATIONS))
        self.output_heads = nn.Conv1d(64, 3, 1)

    def forward(self, x):
        return self.output_heads(self.temporal_blocks(self.input_projection(x)))

    def initialize_legacy(self, state):
        """Start with identical predictions; new context/inputs learn gradually."""
        legacy = NoteBoundaryNet()
        legacy.load_state_dict(state)
        self.temporal_blocks[:4].load_state_dict(legacy.temporal_blocks.state_dict())
        self.output_heads.load_state_dict(legacy.output_heads.state_dict())
        with torch.no_grad():
            self.input_projection[0].weight.zero_()
            self.input_projection[0].weight[:, :MELS].copy_(legacy.input_projection[0].weight)
            self.input_projection[0].bias.copy_(legacy.input_projection[0].bias)
            for block in self.temporal_blocks[4:]:
                block.mix.weight.zero_()
                block.mix.bias.zero_()


def make_model(schema):
    from . import voicing, pitch_structure
    if schema == pitch_structure.SCHEMA: return pitch_structure.PitchStructureNoteBoundaryNet()
    if schema == voicing.SCHEMA: return voicing.VoicingNoteBoundaryNet()
    if schema == SCHEMA: return MultiScaleNoteBoundaryNet()
    if schema == LEGACY_SCHEMA: return NoteBoundaryNet()
    raise ValueError(f"Unsupported note model schema: {schema}")


def make_features(audio, rate, schema, analysis_frames=None):
    from . import voicing, pitch_structure
    if schema in (voicing.SCHEMA, pitch_structure.SCHEMA):
        if analysis_frames is None: raise ValueError("Relative-pitch inputs require native YIN frames")
        base = multiscale_features(audio, rate)
        arrays = [base, voicing.voicing_features(analysis_frames, base.shape[1])]
        if schema == pitch_structure.SCHEMA:
            arrays.append(pitch_structure.structure_features(analysis_frames, base.shape[1]))
        return np.concatenate(arrays)
    if schema == SCHEMA: return multiscale_features(audio, rate)
    if schema == LEGACY_SCHEMA: return features(audio, rate)
    raise ValueError(f"Unsupported note feature schema: {schema}")


def feature_metadata():
    return {"schema": SCHEMA, "feature_contract": CONTRACT, "input_features": FEATURES,
            "long_n_fft": LONG_FFT, "long_mel_filters": long_filters().flatten().tolist(),
            "extra_features": ["spectral_flatness", "high_frequency_ratio", "log_energy"],
            "dilations": list(DILATIONS)}
