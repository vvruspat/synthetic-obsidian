"""
Adversarial disentanglement losses for DDSP Neural Vocal Tuner.

GradientReversalFunction — autograd function for GRL
F0Predictor              — adversarial head: predicts F0 from z_timbre (should fail)
DisentanglementLoss      — combines GRL + F0Predictor for disentanglement training
"""

from __future__ import annotations

import torch
import torch.nn as nn
import torch.nn.functional as F
from einops import rearrange


# ---------------------------------------------------------------------------
# Gradient Reversal Function
# ---------------------------------------------------------------------------

class GradientReversalFunction(torch.autograd.Function):
    """Gradient Reversal Layer (Ganin et al., 2015).

    Forward: identity.
    Backward: multiply gradient by -lambda_ to reverse optimisation direction.

    This forces the encoder to remove information that the adversary (F0Predictor)
    can exploit, effectively making z_timbre independent of F0.
    """

    @staticmethod
    def forward(ctx, x: torch.Tensor, lambda_: float) -> torch.Tensor:  # type: ignore[override]
        ctx.save_for_backward(torch.tensor(lambda_, dtype=torch.float32))
        return x.clone()

    @staticmethod
    def backward(ctx, grad_output: torch.Tensor):  # type: ignore[override]
        (lambda_,) = ctx.saved_tensors
        # Reverse and scale the gradient
        return -lambda_.item() * grad_output, None

    @classmethod
    def apply(cls, x: torch.Tensor, lambda_: float) -> torch.Tensor:  # type: ignore[override]
        return super().apply(x, lambda_)  # type: ignore[return-value]


def gradient_reversal(x: torch.Tensor, lambda_: float = 1.0) -> torch.Tensor:
    """Functional interface for gradient reversal."""
    return GradientReversalFunction.apply(x, lambda_)


# ---------------------------------------------------------------------------
# F0 Predictor (adversary)
# ---------------------------------------------------------------------------

class F0Predictor(nn.Module):
    """Small MLP that attempts to predict F0 from z_timbre.

    Used as the adversary in disentanglement training.  The TimbreEncoder is
    trained (via GRL) to produce z_timbre from which F0 CANNOT be predicted.

    Args:
        timbre_dim:  Dimension of z_timbre input (default: 32).
        hidden_dim:  Hidden layer width (default: 64).
        n_layers:    Number of hidden layers (default: 2).
    """

    def __init__(
        self,
        timbre_dim: int = 32,
        hidden_dim: int = 64,
        n_layers: int = 2,
    ) -> None:
        super().__init__()

        layers: list[nn.Module] = [
            nn.Conv1d(timbre_dim, hidden_dim, kernel_size=1),
            nn.GELU(),
        ]
        for _ in range(n_layers - 1):
            layers += [
                nn.Conv1d(hidden_dim, hidden_dim, kernel_size=1),
                nn.GELU(),
            ]
        # Output: single F0 value per frame (in Hz, normalised by 1000 for stability)
        layers.append(nn.Conv1d(hidden_dim, 1, kernel_size=1))
        self.net = nn.Sequential(*layers)

    def forward(self, z_timbre: torch.Tensor) -> torch.Tensor:
        """Predict F0 from timbre latent.

        Args:
            z_timbre: [B, timbre_dim, T_down]

        Returns:
            f0_pred: [B, T_down] predicted F0 (raw, unnormalised).
        """
        out = self.net(z_timbre)   # [B, 1, T_down]
        return out.squeeze(1)       # [B, T_down]


# ---------------------------------------------------------------------------
# DisentanglementLoss
# ---------------------------------------------------------------------------

class DisentanglementLoss(nn.Module):
    """Adversarial disentanglement: enforce z_timbre ⊥ F0.

    Training procedure:
      1. TimbreEncoder forward pass → z_timbre.
      2. Apply GRL to z_timbre → z_timbre_reversed.
      3. F0Predictor(z_timbre_reversed) → f0_pred.
      4. Compute MSE(f0_pred, f0_true).
      5. Back-propagating this loss:
           - Trains F0Predictor to minimise MSE (better F0 prediction).
           - Trains TimbreEncoder (via reversed gradient) to maximise MSE (remove F0 info).

    Args:
        timbre_dim:    Dimension of z_timbre (default: 32).
        hidden_dim:    F0Predictor hidden width (default: 64).
        lambda_:       GRL reversal strength (default: 0.01).
        warmup_steps:  Steps before ramping up lambda_ to full value (default: 1000).
    """

    def __init__(
        self,
        timbre_dim: int = 32,
        hidden_dim: int = 64,
        lambda_: float = 0.01,
        warmup_steps: int = 1000,
    ) -> None:
        super().__init__()
        self.lambda_ = lambda_
        self.warmup_steps = warmup_steps
        self._step = 0

        self.f0_predictor = F0Predictor(timbre_dim=timbre_dim, hidden_dim=hidden_dim)

    def _current_lambda(self) -> float:
        """Linearly ramp up lambda_ over warmup_steps for training stability."""
        if self.warmup_steps <= 0:
            return self.lambda_
        progress = min(1.0, self._step / self.warmup_steps)
        return self.lambda_ * progress

    def step(self) -> None:
        """Increment internal step counter (call once per training step)."""
        self._step += 1

    def forward(
        self,
        z_timbre: torch.Tensor,
        f0_target: torch.Tensor,
        voiced_mask: torch.Tensor | None = None,
    ) -> torch.Tensor:
        """Compute adversarial disentanglement loss.

        Args:
            z_timbre:    [B, timbre_dim, T_down] timbre latent from TimbreEncoder.
            f0_target:   [B, T_frames] ground-truth F0 in Hz.
            voiced_mask: [B, T_frames] bool, voiced frames only. None = all frames.

        Returns:
            loss: scalar adversarial disentanglement loss.
        """
        lambda_ = self._current_lambda()

        # Apply GRL: gradient is reversed when flowing back to TimbreEncoder
        z_reversed = gradient_reversal(z_timbre, lambda_)  # [B, timbre_dim, T_down]

        # Adversary predicts F0 from reversed latent
        f0_pred = self.f0_predictor(z_reversed)  # [B, T_down]

        # Align temporal dimensions (z_timbre is at 1/4 frame rate of f0_target)
        T_down = f0_pred.shape[-1]
        T_f0 = f0_target.shape[-1]

        if T_f0 != T_down:
            # Downsample f0_target to match z_timbre temporal resolution
            f0_target_ds = F.interpolate(
                f0_target.unsqueeze(1).float(),
                size=T_down,
                mode="linear",
                align_corners=False,
            ).squeeze(1)  # [B, T_down]
        else:
            f0_target_ds = f0_target.float()

        # Normalise F0 to [0, 1] range (divide by 1000 Hz as rough scale)
        f0_target_norm = f0_target_ds / 1000.0
        f0_pred_norm = f0_pred / 1000.0

        # Apply voiced mask if provided (downsample to T_down)
        if voiced_mask is not None:
            if voiced_mask.shape[-1] != T_down:
                # Downsample mask via nearest interpolation
                voiced_mask_ds = F.interpolate(
                    voiced_mask.float().unsqueeze(1),
                    size=T_down,
                    mode="nearest",
                ).squeeze(1).bool()  # [B, T_down]
            else:
                voiced_mask_ds = voiced_mask

            if voiced_mask_ds.sum() == 0:
                return torch.tensor(0.0, device=z_timbre.device)

            f0_pred_voiced = f0_pred_norm[voiced_mask_ds]
            f0_tgt_voiced = f0_target_norm[voiced_mask_ds]
            loss = F.mse_loss(f0_pred_voiced, f0_tgt_voiced)
        else:
            loss = F.mse_loss(f0_pred_norm, f0_target_norm)

        return loss
