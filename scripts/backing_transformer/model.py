"""Small non-autoregressive Transformer for backing-vocal pitch offsets."""

from __future__ import annotations

from dataclasses import dataclass
import math
from typing import Any

import mlx.core as mx
import mlx.nn as nn


OFFSET_MIN = -24
OFFSET_MAX = 24
OFFSET_CLASSES = OFFSET_MAX - OFFSET_MIN + 1
CONTINUOUS_FEATURES = 20


@dataclass(frozen=True)
class ModelConfig:
    styles: tuple[str, ...]
    dimensions: int = 96
    heads: int = 4
    layers: int = 4
    mlp_dimensions: int = 192
    max_notes: int = 64

    def as_dict(self) -> dict[str, Any]:
        return {
            "schema": "synthetic-obsidian-backing-transformer-v1",
            "styles": list(self.styles),
            "dimensions": self.dimensions,
            "heads": self.heads,
            "layers": self.layers,
            "mlp_dimensions": self.mlp_dimensions,
            "max_notes": self.max_notes,
            "offset_min": OFFSET_MIN,
            "offset_max": OFFSET_MAX,
        }


class BackingTransformer(nn.Module):
    def __init__(self, config: ModelConfig):
        super().__init__()
        dims = config.dimensions
        self.pitch = nn.Embedding(128, dims)
        self.style = nn.Embedding(len(config.styles), dims)
        self.chord_root = nn.Embedding(13, dims)
        self.chord_quality = nn.Embedding(16, dims)
        self.key_pc = nn.Embedding(13, dims)
        self.key_mode = nn.Embedding(3, dims)
        self.position = nn.Embedding(config.max_notes, dims)
        self.continuous = nn.Linear(CONTINUOUS_FEATURES, dims)
        self.encoder = nn.TransformerEncoder(
            num_layers=config.layers,
            dims=dims,
            num_heads=config.heads,
            mlp_dims=config.mlp_dimensions,
            dropout=0.0,
            norm_first=True,
        )
        self.output = nn.Linear(dims, OFFSET_CLASSES)

    def __call__(
        self,
        pitch: mx.array,
        style: mx.array,
        chord_root: mx.array,
        chord_quality: mx.array,
        key_pc: mx.array,
        key_mode: mx.array,
        continuous: mx.array,
        valid: mx.array,
    ) -> mx.array:
        batch, length = pitch.shape
        positions = mx.broadcast_to(mx.arange(length)[None, :], (batch, length))
        x = (
            self.pitch(pitch)
            + self.style(style)[:, None, :]
            + self.chord_root(chord_root)
            + self.chord_quality(chord_quality)
            + self.key_pc(key_pc)[:, None, :]
            + self.key_mode(key_mode)[:, None, :]
            + self.position(positions)
            + self.continuous(continuous)
        )
        attention_mask = mx.where(valid[:, None, None, :], mx.array(0.0), mx.array(-1.0e9))
        return self.output(self.encoder(x, attention_mask))


def continuous_features(note: dict[str, Any]) -> list[float]:
    chord_mask = int(note["chord_mask"])
    beat = float(note["beat"])
    meter = max(1.0, float(note["meter"]))
    beat_phase = 2.0 * 3.141592653589793 * ((beat - 1.0) % meter) / meter
    features = [
        min(float(note["duration"]), 8.0) / 8.0,
        min(float(note["gap"]), 4.0) / 4.0,
        float(note["lead_delta"]) / 24.0,
        float(note["velocity"]) / 127.0,
        1.0 if note["phrase_start"] else 0.0,
        math.sin(beat_phase),
        math.cos(beat_phase),
        min(meter, 12.0) / 12.0,
    ]
    features.extend(1.0 if chord_mask & (1 << pc) else 0.0 for pc in range(12))
    return features
