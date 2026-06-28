from __future__ import annotations

import torch
import torch.nn as nn
import torch.nn.functional as F


class ConvBlock(nn.Module):
    def __init__(self, channels: int, kernel_size: int, dilation: int = 1) -> None:
        super().__init__()
        padding = ((kernel_size - 1) // 2) * dilation
        self.block = nn.Sequential(
            nn.Conv1d(channels, channels, kernel_size, padding=padding, dilation=dilation),
            nn.BatchNorm1d(channels),
            nn.GELU(),
        )

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return x + self.block(x)


class NoteDetectorModel(nn.Module):
    def __init__(
        self,
        n_mels: int = 80,
        hidden_dim: int = 160,
        num_blocks: int = 6,
        n_pitch_bins: int = 49,
    ) -> None:
        super().__init__()
        self.input_proj = nn.Sequential(
            nn.Conv1d(n_mels, hidden_dim, kernel_size=3, padding=1),
            nn.BatchNorm1d(hidden_dim),
            nn.GELU(),
        )
        dilations = [1, 2, 4, 8, 1, 2][:num_blocks]
        self.backbone = nn.Sequential(*[ConvBlock(hidden_dim, kernel_size=5, dilation=d) for d in dilations])
        self.voiced_head = nn.Conv1d(hidden_dim, 1, kernel_size=1)
        self.onset_head = nn.Conv1d(hidden_dim, 1, kernel_size=1)
        self.pitch_head = nn.Conv1d(hidden_dim, n_pitch_bins, kernel_size=1)

    def forward(self, log_mel: torch.Tensor) -> dict[str, torch.Tensor]:
        x = self.input_proj(log_mel)
        x = self.backbone(x)
        return {
            "voiced_logits": self.voiced_head(x).squeeze(1),
            "onset_logits": self.onset_head(x).squeeze(1),
            "pitch_logits": self.pitch_head(x),
        }


class NoteDetectorLoss(nn.Module):
    def __init__(
        self,
        voiced_weight: float = 1.0,
        onset_weight: float = 2.0,
        pitch_weight: float = 1.0,
        pitch_label_smoothing: float = 0.05,
    ) -> None:
        super().__init__()
        self.voiced_weight = voiced_weight
        self.onset_weight = onset_weight
        self.pitch_weight = pitch_weight
        self.pitch_label_smoothing = pitch_label_smoothing

    def forward(
        self,
        predictions: dict[str, torch.Tensor],
        batch: dict[str, torch.Tensor | list[str]],
    ) -> dict[str, torch.Tensor]:
        frame_mask = batch["frame_mask"]  # type: ignore[assignment]
        voiced_target = batch["voiced"]  # type: ignore[assignment]
        onset_target = batch["onset"]  # type: ignore[assignment]
        pitch_target = batch["pitch_class"]  # type: ignore[assignment]
        pitch_mask = batch["pitch_mask"]  # type: ignore[assignment]

        voiced_loss = F.binary_cross_entropy_with_logits(
            predictions["voiced_logits"],
            voiced_target,
            reduction="none",
        )
        voiced_loss = (voiced_loss * frame_mask).sum() / frame_mask.sum().clamp_min(1.0)

        onset_loss = F.binary_cross_entropy_with_logits(
            predictions["onset_logits"],
            onset_target,
            reduction="none",
        )
        onset_loss = (onset_loss * frame_mask).sum() / frame_mask.sum().clamp_min(1.0)

        pitch_loss_map = F.cross_entropy(
            predictions["pitch_logits"],
            pitch_target,
            ignore_index=-100,
            reduction="none",
            label_smoothing=self.pitch_label_smoothing,
        )
        pitch_active = pitch_mask * frame_mask
        pitch_loss = (pitch_loss_map * pitch_active).sum() / pitch_active.sum().clamp_min(1.0)

        total = (
            self.voiced_weight * voiced_loss
            + self.onset_weight * onset_loss
            + self.pitch_weight * pitch_loss
        )
        return {
            "total": total,
            "voiced": voiced_loss,
            "onset": onset_loss,
            "pitch": pitch_loss,
        }
