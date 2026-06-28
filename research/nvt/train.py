"""
NVT Training script with Hydra configuration.

Run:
    # From research/ directory:
    python -m nvt.train  # uses configs/model/default.yaml etc.
    python -m nvt.train train.lr=5e-5 train.phase=1
    python -m nvt.train +train.phase=2 model.freeze_vocos=false
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import Any, Dict, Optional

import torch
import torch.nn as nn
import torch.nn.functional as F
import torch.optim as optim
from torch.utils.data import DataLoader, Dataset
import hydra
from omegaconf import DictConfig, OmegaConf


# ---------------------------------------------------------------------------
# Placeholder dataset
# ---------------------------------------------------------------------------

class _RandomTensorDataset(Dataset):
    """Placeholder dataset returning random tensors with correct shapes.

    Replace this with a real dataset reading from prepare_vocalset.py output.

    Args:
        n_samples:    Number of samples (default: 256 for quick iteration).
        sample_rate:  Audio sample rate (default: 24000).
        duration_s:   Segment duration in seconds (default: 4.0).
        hop_length:   Analysis hop (default: 256).
        n_mels:       Mel bins (default: 80).
    """

    def __init__(
        self,
        n_samples: int = 256,
        sample_rate: int = 24000,
        duration_s: float = 4.0,
        hop_length: int = 256,
        n_mels: int = 80,
    ) -> None:
        self.n_samples = n_samples
        self.n_audio = int(sample_rate * duration_s)
        self.n_frames = self.n_audio // hop_length
        self.n_mels = n_mels
        self.sample_rate = sample_rate

    def __len__(self) -> int:
        return self.n_samples

    def __getitem__(self, idx: int) -> Dict[str, torch.Tensor]:
        audio = torch.randn(self.n_audio)  # [T]
        f0_hz = torch.rand(self.n_frames) * 300 + 80  # [T_frames] in [80, 380] Hz
        voiced = (torch.rand(self.n_frames) > 0.3).float()  # ~70% voiced
        f0_hz = f0_hz * voiced  # zero out unvoiced
        loudness_db = torch.randn(self.n_frames) * 10 - 20  # [T_frames] dB

        return {
            "audio": audio,
            "f0_hz": f0_hz,
            "voiced": voiced.bool(),
            "loudness_db": loudness_db,
            "sample_rate": self.sample_rate,
        }


# ---------------------------------------------------------------------------
# Loss aggregator
# ---------------------------------------------------------------------------

class NVTLoss(nn.Module):
    """Combine all losses according to phase and config weights."""

    def __init__(
        self,
        lambda_spec: float = 1.0,
        lambda_wave: float = 1.0,
        lambda_rms: float = 2.0,
        lambda_peak: float = 10.0,
        lambda_f0: float = 0.1,
        lambda_disent: float = 0.01,
        lambda_perc: float = 0.0,
        phase: int = 0,
    ) -> None:
        super().__init__()
        self.lambda_spec = lambda_spec
        self.lambda_wave = lambda_wave
        self.lambda_rms = lambda_rms
        self.lambda_peak = lambda_peak
        self.lambda_f0 = lambda_f0
        self.lambda_disent = lambda_disent
        self.lambda_perc = lambda_perc
        self.phase = phase

        # Import losses lazily to allow standalone module usage
        from nvt.losses.spectral import MultiScaleSpectralLoss
        self.spectral_loss = MultiScaleSpectralLoss()

        if phase >= 1:
            from nvt.losses.adversarial import DisentanglementLoss
            self.disent_loss = DisentanglementLoss(lambda_=lambda_disent)
        else:
            self.disent_loss = None  # type: ignore

    def forward(
        self,
        predicted_audio: torch.Tensor,
        target_audio: torch.Tensor,
        z_timbre: torch.Tensor,
        f0_target: torch.Tensor,
        voiced_mask: Optional[torch.Tensor] = None,
    ) -> Dict[str, torch.Tensor]:
        """Compute all active losses.

        Returns:
            Dict with individual loss values and 'total'.
        """
        losses: Dict[str, torch.Tensor] = {}

        # Multi-scale spectral loss (always active)
        spec_loss = self.spectral_loss(predicted_audio, target_audio)
        losses["spectral"] = spec_loss

        total = self.lambda_spec * spec_loss

        # Waveform and amplitude losses keep the model honest about absolute
        # signal scale. Multi-scale STFT alone can reward clipped, over-loud
        # reconstructions if their spectral envelope is close enough.
        min_len = min(predicted_audio.shape[-1], target_audio.shape[-1])
        predicted_trimmed = predicted_audio[:, :min_len]
        target_trimmed = target_audio[:, :min_len]

        wave_loss = F.l1_loss(predicted_trimmed, target_trimmed)
        losses["waveform"] = wave_loss
        total = total + self.lambda_wave * wave_loss

        eps = 1e-7
        pred_rms = torch.sqrt(predicted_trimmed.pow(2).mean(dim=-1) + eps)
        target_rms = torch.sqrt(target_trimmed.pow(2).mean(dim=-1) + eps)
        rms_loss = F.l1_loss(torch.log(pred_rms + eps), torch.log(target_rms + eps))
        losses["rms"] = rms_loss
        total = total + self.lambda_rms * rms_loss

        peak_loss = F.relu(predicted_trimmed.abs() - 0.98).pow(2).mean()
        losses["peak"] = peak_loss
        total = total + self.lambda_peak * peak_loss

        # Disentanglement (Phase 1+)
        if self.phase >= 1 and self.disent_loss is not None:
            disent = self.disent_loss(z_timbre, f0_target, voiced_mask)
            losses["disentanglement"] = disent
            total = total + self.lambda_disent * disent
            self.disent_loss.step()

        losses["total"] = total
        return losses


# ---------------------------------------------------------------------------
# Training loop
# ---------------------------------------------------------------------------

def load_checkpoint(
    model: nn.Module,
    optimizer: optim.Optimizer,
    checkpoint_dir: Path,
    device: torch.device,
    resume_from: Optional[str] = None,
    allow_partial: bool = True,
) -> tuple[int, int]:
    """Load a checkpoint if available. Returns (start_epoch, global_step)."""
    if resume_from:
        latest = Path(resume_from)
    else:
        ckpts = sorted(checkpoint_dir.glob("epoch_*.pt"))
        latest = ckpts[-1] if ckpts else None

    if latest is None:
        return 0, 0

    print(f"  Resuming from checkpoint: {latest}")
    state = torch.load(str(latest), map_location=device)
    model_state = state.get("model", state) if isinstance(state, dict) else state

    if not isinstance(model_state, dict):
        raise ValueError(f"Checkpoint does not contain a model state dict: {latest}")

    current_state = model.state_dict()
    compatible_state = {}
    skipped = []
    for key, value in model_state.items():
        if key in current_state and current_state[key].shape == value.shape:
            compatible_state[key] = value
        else:
            skipped.append(key)

    if skipped and not allow_partial:
        preview = ", ".join(skipped[:8])
        raise RuntimeError(f"Checkpoint has incompatible tensors: {preview}")

    if skipped:
        print(f"  Partial resume: loaded {len(compatible_state)}/{len(current_state)} tensors.")
        print("  Skipped incompatible tensors:")
        for key in skipped[:12]:
            old_shape = tuple(model_state[key].shape) if hasattr(model_state[key], "shape") else "?"
            new_shape = tuple(current_state[key].shape) if key in current_state else "missing"
            print(f"    {key}: checkpoint {old_shape} -> model {new_shape}")
        if len(skipped) > 12:
            print(f"    ... {len(skipped) - 12} more")
    else:
        print(f"  Loaded all {len(compatible_state)} model tensors.")

    model.load_state_dict(compatible_state, strict=False)

    if isinstance(state, dict) and "optimizer" in state and not skipped:
        optimizer.load_state_dict(state["optimizer"])
    elif isinstance(state, dict) and "optimizer" in state:
        print("  Optimizer state skipped because model architecture changed.")

    start_epoch = state.get("epoch", -1) + 1 if isinstance(state, dict) else 0
    global_step = state.get("global_step", 0) if isinstance(state, dict) else 0
    return start_epoch, global_step


def save_checkpoint(
    model: nn.Module,
    optimizer: optim.Optimizer,
    epoch: int,
    global_step: int,
    checkpoint_dir: Path,
) -> None:
    """Save training checkpoint."""
    checkpoint_dir.mkdir(parents=True, exist_ok=True)
    path = checkpoint_dir / f"epoch_{epoch:04d}.pt"
    torch.save(
        {
            "model": model.state_dict(),
            "optimizer": optimizer.state_dict(),
            "epoch": epoch,
            "global_step": global_step,
        },
        str(path),
    )
    print(f"  Checkpoint → {path}")


# ---------------------------------------------------------------------------
# Hydra entry point
# ---------------------------------------------------------------------------

@hydra.main(version_base=None, config_path="../configs", config_name="train")
def train(cfg: DictConfig) -> None:
    """Main training function, driven by Hydra config.

    Config structure expected:
        cfg.model.*   — model hyperparameters
        cfg.train.*   — training hyperparameters
        cfg.data.*    — dataset config
        cfg.phase     — training phase 0–3
    """
    print(OmegaConf.to_yaml(cfg))

    device = torch.device("cuda" if torch.cuda.is_available() else "cpu")
    print(f"Device: {device}")

    phase = int(getattr(cfg, "phase", 0))
    print(f"Training phase: {phase}")

    # --- Model ---
    from nvt.models.full_model import NeuralVocalTuner

    model_cfg = cfg.get("model", {})
    model = NeuralVocalTuner(
        sample_rate=int(getattr(model_cfg, "sample_rate", 24000)),
        hop_length=int(getattr(model_cfg, "hop_length", 256)),
        n_fft=int(getattr(model_cfg, "n_fft", 1024)),
        n_mels=int(getattr(model_cfg, "n_mels", 80)),
        timbre_dim=int(getattr(model_cfg, "timbre_dim", 32)),
        noise_dim=int(getattr(model_cfg, "noise_dim", 16)),
        n_harmonics=int(getattr(model_cfg, "n_harmonics", 100)),
        noise_filter_len=int(getattr(model_cfg, "noise_filter_len", 64)),
        freeze_vocos=bool(getattr(model_cfg, "freeze_vocos", True)),
    ).to(device)

    # Phase 2+: unfreeze Vocos
    if phase >= 2 and model.vocos is not None:
        for p in model.vocos.parameters():
            p.requires_grad_(True)
        print("  Vocos unfrozen for fine-tuning (Phase 2+).")

    # --- Optimizer ---
    train_cfg = cfg.get("train", {})
    lr = float(getattr(train_cfg, "lr", 1e-4))
    grad_clip = float(getattr(train_cfg, "grad_clip", 1.0))
    epochs = int(getattr(train_cfg, "epochs", 100))
    batch_size = int(getattr(train_cfg, "batch_size", 16))
    log_every = int(getattr(train_cfg, "log_every_n_steps", 50))
    ckpt_every = int(getattr(train_cfg, "checkpoint_every_n_epochs", 10))
    max_steps_per_epoch = getattr(train_cfg, "max_steps_per_epoch", None)
    max_steps_per_epoch = int(max_steps_per_epoch) if max_steps_per_epoch is not None else None
    resume_from = getattr(train_cfg, "resume_from", None)
    allow_partial_resume = bool(getattr(train_cfg, "allow_partial_resume", True))

    optimizer = optim.AdamW(
        [p for p in model.parameters() if p.requires_grad],
        lr=lr,
        betas=(0.9, 0.999),
        weight_decay=1e-4,
    )

    # --- Loss ---
    criterion = NVTLoss(
        lambda_spec=float(getattr(train_cfg, "lambda_spec", 1.0)),
        lambda_wave=float(getattr(train_cfg, "lambda_wave", 1.0)),
        lambda_rms=float(getattr(train_cfg, "lambda_rms", 2.0)),
        lambda_peak=float(getattr(train_cfg, "lambda_peak", 10.0)),
        lambda_f0=float(getattr(train_cfg, "lambda_f0", 0.1)),
        lambda_disent=float(getattr(train_cfg, "lambda_disent", 0.01)),
        lambda_perc=float(getattr(train_cfg, "lambda_perc", 0.0)),
        phase=phase,
    ).to(device)

    # --- Data ---
    data_cfg = cfg.get("data", {})
    sr = int(getattr(data_cfg, "sample_rate", 24000))
    seg_dur = float(getattr(data_cfg, "segment_duration", 4.0))
    manifest = getattr(data_cfg, "manifest", None)

    if manifest and Path(manifest).exists():
        from nvt.data.dataset import NVTDataset
        dataset = NVTDataset(
            manifest_path=manifest,
            segment_seconds=seg_dur,
            sample_rate=sr,
            hop_length=int(getattr(model_cfg, "hop_length", 256)),
            n_mels=int(getattr(model_cfg, "n_mels", 80)),
        )
        print(f"  Using real dataset: {manifest} ({len(dataset)} samples)")
    else:
        dataset = _RandomTensorDataset(
            n_samples=512,
            sample_rate=sr,
            duration_s=seg_dur,
        )
        print("  Using synthetic random dataset (no manifest found).")

    loader = DataLoader(
        dataset,
        batch_size=batch_size,
        shuffle=True,
        num_workers=min(4, os.cpu_count() or 1),
        pin_memory=device.type == "cuda",
        drop_last=True,
    )

    # --- Checkpoint dir ---
    checkpoint_dir = Path(str(getattr(train_cfg, "checkpoint_dir", "checkpoints")))
    start_epoch, global_step = load_checkpoint(
        model,
        optimizer,
        checkpoint_dir,
        device,
        resume_from=resume_from,
        allow_partial=allow_partial_resume,
    )

    # --- WandB (optional) ---
    wandb_run = None
    try:
        import wandb  # type: ignore
        wandb_run = wandb.init(
            project="synthetic-obsidian-nvt",
            name=f"phase{phase}",
            config=OmegaConf.to_container(cfg, resolve=True),
            resume="allow",
        )
    except Exception:
        print("  wandb not available — logging to stdout only.")

    # --- Training loop ---
    print(f"\nStarting training: epochs={epochs}, batch_size={batch_size}, lr={lr}")
    model.train()

    if start_epoch >= epochs:
        print(f"Checkpoint is already at epoch {start_epoch}; nothing to train for epochs={epochs}.")
        if wandb_run is not None:
            wandb_run.finish()
        return

    last_epoch = None
    for epoch in range(start_epoch, epochs):
        last_epoch = epoch
        epoch_losses: Dict[str, float] = {}

        n_batches = 0
        for batch in loader:
            audio     = batch["audio"].to(device)           # [B, T]
            f0_target = batch["f0_hz"].to(device)           # [B, T_frames]
            voiced    = batch["voiced"].to(device)          # [B, T_frames]
            # Use pre-computed features when available (skips torchcrepe + loudness re-compute)
            log_mel   = batch.get("log_mel")
            loudness  = batch.get("loudness_db")
            if log_mel  is not None: log_mel  = log_mel.to(device)
            if loudness is not None: loudness = loudness.to(device)

            optimizer.zero_grad()

            # Forward pass — pre-computed F0 + mel avoids torchcrepe at training time
            outputs = model.forward(
                audio,
                f0_target=f0_target,
                log_mel=log_mel,
                loudness=loudness,
            )
            pred_audio = outputs["waveform"]
            z_timbre = outputs["z_timbre"]

            # Compute losses
            loss_dict = criterion(
                predicted_audio=pred_audio,
                target_audio=audio,
                z_timbre=z_timbre,
                f0_target=f0_target,
                voiced_mask=voiced,
            )
            total_loss = loss_dict["total"]

            total_loss.backward()

            # Gradient clipping
            nn.utils.clip_grad_norm_(model.parameters(), grad_clip)

            optimizer.step()

            # Accumulate for epoch-level logging
            for k, v in loss_dict.items():
                epoch_losses[k] = epoch_losses.get(k, 0.0) + v.item()

            global_step += 1
            n_batches += 1

            # Step-level logging
            if global_step % log_every == 0:
                log_str = f"  step={global_step:6d}  " + "  ".join(
                    f"{k}={v.item():.4f}" for k, v in loss_dict.items()
                )
                print(log_str)
                if wandb_run is not None:
                    wandb_run.log({f"step/{k}": v.item() for k, v in loss_dict.items()}, step=global_step)

            if max_steps_per_epoch is not None and n_batches >= max_steps_per_epoch:
                break

        # Epoch-level logging
        epoch_avg = {k: v / n_batches for k, v in epoch_losses.items()}
        print(
            f"Epoch {epoch:4d}/{epochs}  "
            + "  ".join(f"{k}={v:.4f}" for k, v in epoch_avg.items())
        )
        if wandb_run is not None:
            wandb_run.log({f"epoch/{k}": v for k, v in epoch_avg.items()}, step=global_step)

        # Checkpoint
        if (epoch + 1) % ckpt_every == 0:
            save_checkpoint(model, optimizer, epoch, global_step, checkpoint_dir)

    # Final checkpoint, unless the checkpoint cadence already wrote this epoch.
    if last_epoch is not None and (last_epoch + 1) % ckpt_every != 0:
        save_checkpoint(model, optimizer, last_epoch, global_step, checkpoint_dir)

    if wandb_run is not None:
        wandb_run.finish()

    print("Training complete.")


if __name__ == "__main__":
    train()
