"""
Spectral losses for DDSP Neural Vocal Tuner.

MultiScaleSpectralLoss — L1 on log-magnitude STFT at multiple FFT scales.
F0ConsistencyLoss      — MSE in cents between output F0 and target F0 (via torchcrepe).
"""

from __future__ import annotations

from typing import List

import torch
import torch.nn as nn
import torch.nn.functional as F


class MultiScaleSpectralLoss(nn.Module):
    """Multi-scale STFT reconstruction loss.

    Computes L1 loss between log-magnitude spectrograms at multiple FFT sizes.
    This captures both fine-grained and coarse spectral structure simultaneously.

    Args:
        fft_sizes:   List of FFT sizes (default: [2048, 1024, 512, 256, 128]).
        hop_ratio:   hop_length = fft_size // hop_ratio (default: 4).
        eps:         Small value for log stability (default: 1e-5).
        weight_log:  If True, add L1 loss on raw magnitude too (default: True).
    """

    def __init__(
        self,
        fft_sizes: List[int] | None = None,
        hop_ratio: int = 4,
        eps: float = 1e-5,
        weight_log: bool = True,
    ) -> None:
        super().__init__()
        self.fft_sizes = fft_sizes or [2048, 1024, 512, 256, 128]
        self.hop_ratio = hop_ratio
        self.eps = eps
        self.weight_log = weight_log

    def _stft_magnitude(
        self, audio: torch.Tensor, n_fft: int, hop_length: int
    ) -> torch.Tensor:
        """Compute magnitude spectrogram.

        Args:
            audio:      [B, T] float32 waveform.
            n_fft:      FFT size.
            hop_length: Hop size.

        Returns:
            mag: [B, n_fft//2+1, T_frames] float32 magnitude.
        """
        window = torch.hann_window(n_fft, device=audio.device)
        stft = torch.stft(
            audio,
            n_fft=n_fft,
            hop_length=hop_length,
            win_length=n_fft,
            window=window,
            return_complex=True,
        )
        return stft.abs()  # [B, F, T_frames]

    def forward(self, predicted: torch.Tensor, target: torch.Tensor) -> torch.Tensor:
        """Compute multi-scale spectral loss.

        Args:
            predicted: [B, T] predicted waveform.
            target:    [B, T] target waveform.

        Returns:
            loss: scalar tensor.
        """
        # Align lengths
        min_len = min(predicted.shape[-1], target.shape[-1])
        predicted = predicted[:, :min_len]
        target = target[:, :min_len]

        total_loss = torch.tensor(0.0, device=predicted.device)

        for n_fft in self.fft_sizes:
            hop = n_fft // self.hop_ratio

            pred_mag = self._stft_magnitude(predicted, n_fft, hop)
            tgt_mag = self._stft_magnitude(target, n_fft, hop)

            # Align frame counts (can differ by 1 due to padding)
            T = min(pred_mag.shape[-1], tgt_mag.shape[-1])
            pred_mag = pred_mag[:, :, :T]
            tgt_mag = tgt_mag[:, :, :T]

            # Log-magnitude L1 loss
            pred_log = torch.log(pred_mag + self.eps)
            tgt_log = torch.log(tgt_mag + self.eps)
            loss_log = F.l1_loss(pred_log, tgt_log)
            total_loss = total_loss + loss_log

            # Raw magnitude L1 loss (optional)
            if self.weight_log:
                loss_mag = F.l1_loss(pred_mag, tgt_mag)
                total_loss = total_loss + loss_mag

        n_terms = len(self.fft_sizes) * (2 if self.weight_log else 1)
        return total_loss / n_terms


class F0ConsistencyLoss(nn.Module):
    """Measure F0 accuracy of synthesised output vs. target F0.

    Uses torchcrepe to re-estimate F0 from the output waveform, then
    computes MSE in cents between estimated and target F0.

    Cents = 1200 * log2(f0_pred / f0_target)  for voiced frames only.

    Args:
        sample_rate:         Audio sample rate (default: 24000).
        hop_length:          Analysis hop (default: 256).
        fmin:                Minimum F0 (default: 50 Hz).
        fmax:                Maximum F0 (default: 1000 Hz).
        model:               torchcrepe variant (default: "tiny" for speed during training).
        confidence_threshold: Unvoiced threshold (default: 0.4).
    """

    def __init__(
        self,
        sample_rate: int = 24000,
        hop_length: int = 256,
        fmin: float = 50.0,
        fmax: float = 1000.0,
        model: str = "tiny",
        confidence_threshold: float = 0.4,
    ) -> None:
        super().__init__()
        self.sample_rate = sample_rate
        self.hop_length = hop_length
        self.fmin = fmin
        self.fmax = fmax
        self.model = model
        self.confidence_threshold = confidence_threshold

    def forward(
        self,
        predicted_audio: torch.Tensor,
        f0_target: torch.Tensor,
        voiced_mask: torch.Tensor | None = None,
    ) -> torch.Tensor:
        """Compute F0 consistency loss.

        Args:
            predicted_audio: [B, T] predicted waveform.
            f0_target:       [B, T_frames] target F0 in Hz.
            voiced_mask:     [B, T_frames] bool, True = voiced. If None, derived from f0_target > 0.

        Returns:
            loss: scalar tensor (MSE in cents, voiced frames only).
        """
        try:
            import torchcrepe  # type: ignore
        except ImportError as exc:
            raise ImportError(
                "torchcrepe is required for F0ConsistencyLoss. Install with: pip install torchcrepe"
            ) from exc

        device = predicted_audio.device

        # Estimate F0 of predicted audio
        f0_list, conf_list = [], []
        for b in range(predicted_audio.shape[0]):
            wav = predicted_audio[b : b + 1].detach()
            f0, conf = torchcrepe.predict(
                wav,
                self.sample_rate,
                self.hop_length,
                self.fmin,
                self.fmax,
                model=self.model,
                return_periodicity=True,
                device=device,
                batch_size=512,
            )
            f0_list.append(f0)
            conf_list.append(conf)

        f0_pred = torch.cat(f0_list, dim=0)      # [B, T_frames_pred]
        confidence = torch.cat(conf_list, dim=0)  # [B, T_frames_pred]

        # Align frame counts
        T = min(f0_pred.shape[-1], f0_target.shape[-1])
        f0_pred = f0_pred[:, :T]
        f0_target_trimmed = f0_target[:, :T]
        confidence = confidence[:, :T]

        # Build voiced mask
        if voiced_mask is None:
            voiced_mask = f0_target_trimmed > 0.0
        else:
            voiced_mask = voiced_mask[:, :T]

        # Also require prediction confidence above threshold
        voiced_mask = voiced_mask & (confidence >= self.confidence_threshold)

        if voiced_mask.sum() == 0:
            return torch.tensor(0.0, device=device, requires_grad=True)

        # Convert to cents: 1200 * log2(f_pred / f_target) — clamp F0 to avoid log(0)
        f0_pred_voiced = f0_pred[voiced_mask].clamp(min=self.fmin)
        f0_tgt_voiced = f0_target_trimmed[voiced_mask].clamp(min=self.fmin)

        cents_error = 1200.0 * torch.log2(f0_pred_voiced / f0_tgt_voiced)
        return F.mse_loss(cents_error, torch.zeros_like(cents_error))
