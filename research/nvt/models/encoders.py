"""
Encoders for the DDSP Neural Vocal Tuner pipeline.

F0Encoder     — torchcrepe wrapper, returns (f0_hz, voiced) at hop_length frame rate
LoudnessEncoder — A-weighted RMS in dB at the same frame rate
TimbreEncoder  — 1D CNN with Gradient Reversal Layer, produces F0-invariant z_timbre
NoiseEncoder   — lighter 1D CNN, produces z_noise for sibilant / breath modelling
"""

from __future__ import annotations

import math
from typing import Tuple

import torch
import torch.nn as nn
import torch.nn.functional as F
from einops import rearrange

# ---------------------------------------------------------------------------
# Gradient Reversal Layer
# ---------------------------------------------------------------------------

class _GradientReversalFunction(torch.autograd.Function):
    """Reverses gradients during backward pass with scale factor lambda_."""

    @staticmethod
    def forward(ctx, x: torch.Tensor, lambda_: float) -> torch.Tensor:  # type: ignore[override]
        ctx.save_for_backward(torch.tensor(lambda_))
        return x.clone()

    @staticmethod
    def backward(ctx, grad_output: torch.Tensor):  # type: ignore[override]
        (lambda_,) = ctx.saved_tensors
        return -lambda_.item() * grad_output, None


class GradientReversalLayer(nn.Module):
    """Apply identity in forward, scale-reversed gradient in backward.

    Args:
        lambda_: Reversal strength (positive float). Typical start: 0.01.
    """

    def __init__(self, lambda_: float = 0.01) -> None:
        super().__init__()
        self.lambda_ = lambda_

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        return _GradientReversalFunction.apply(x, self.lambda_)

    def extra_repr(self) -> str:
        return f"lambda_={self.lambda_}"


# ---------------------------------------------------------------------------
# F0 Encoder
# ---------------------------------------------------------------------------

class F0Encoder(nn.Module):
    """Estimate fundamental frequency using torchcrepe.

    Returns (f0_hz, voiced) at the requested hop_length frame rate.

    Args:
        sample_rate:         Audio sample rate in Hz (default: 24000).
        hop_length:          Analysis hop in samples (default: 256 → ~5.3 ms at 24 kHz).
        fmin:                Minimum F0 in Hz (default: 50 Hz).
        fmax:                Maximum F0 in Hz (default: 1000 Hz).
        model:               torchcrepe model variant: "full" or "tiny" (default: "full").
        confidence_threshold: Frames below this confidence are marked unvoiced (default: 0.4).
        device:              Torch device (inferred from input if None).
    """

    def __init__(
        self,
        sample_rate: int = 24000,
        hop_length: int = 256,
        fmin: float = 50.0,
        fmax: float = 1000.0,
        model: str = "full",
        confidence_threshold: float = 0.4,
        device: str | torch.device | None = None,
    ) -> None:
        super().__init__()
        self.sample_rate = sample_rate
        self.hop_length = hop_length
        self.fmin = fmin
        self.fmax = fmax
        self.model = model
        self.confidence_threshold = confidence_threshold
        self._device = device

    @torch.no_grad()
    def forward(self, audio: torch.Tensor) -> Tuple[torch.Tensor, torch.Tensor]:
        """Estimate F0 for a batch of waveforms.

        Args:
            audio: [B, T] float32, mono waveforms at self.sample_rate.

        Returns:
            f0_hz:  [B, T_frames] float32 — fundamental frequency in Hz.
            voiced: [B, T_frames] bool    — True where voice is detected.
        """
        try:
            import torchcrepe  # type: ignore
        except ImportError as exc:
            raise ImportError("torchcrepe is required for F0Encoder. Install with: pip install torchcrepe") from exc

        device = audio.device if self._device is None else torch.device(self._device)

        f0_list, conf_list = [], []
        for b in range(audio.shape[0]):
            # torchcrepe expects [1, T]
            wav = audio[b : b + 1].to(device)
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
            f0_list.append(f0)       # [1, T_frames]
            conf_list.append(conf)   # [1, T_frames]

        f0_hz = torch.cat(f0_list, dim=0)    # [B, T_frames]
        confidence = torch.cat(conf_list, dim=0)  # [B, T_frames]
        voiced = confidence >= self.confidence_threshold  # [B, T_frames] bool

        # Zero out F0 in unvoiced regions for clean synthesis
        f0_hz = f0_hz * voiced.float()

        return f0_hz, voiced


# ---------------------------------------------------------------------------
# Loudness Encoder
# ---------------------------------------------------------------------------

# A-weighting filter coefficients at common sample rates.
# Implemented as approximate gain curve applied in frequency domain.
_A_WEIGHT_CENTER_FREQS = torch.tensor(
    [31.5, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000], dtype=torch.float32
)
_A_WEIGHT_GAINS_DB = torch.tensor(
    [-39.4, -26.2, -16.1, -8.6, -3.2, 0.0, 1.2, 1.0, -1.1, -6.6], dtype=torch.float32
)


class LoudnessEncoder(nn.Module):
    """Compute A-weighted loudness in dB at F0 frame rate.

    Args:
        sample_rate: Audio sample rate in Hz.
        hop_length:  Analysis hop in samples.
        n_fft:       FFT size (default: 1024).
        ref_db:      Reference level in dB (default: -20.0).
    """

    def __init__(
        self,
        sample_rate: int = 24000,
        hop_length: int = 256,
        n_fft: int = 1024,
        ref_db: float = -20.0,
    ) -> None:
        super().__init__()
        self.sample_rate = sample_rate
        self.hop_length = hop_length
        self.n_fft = n_fft
        self.ref_db = ref_db

        # Build A-weighting curve in frequency domain (non-trainable)
        freqs = torch.linspace(0, sample_rate / 2, n_fft // 2 + 1)
        a_weights = self._a_weighting(freqs)  # [n_fft//2+1]
        self.register_buffer("a_weights_linear", a_weights)

    @staticmethod
    def _a_weighting(freqs: torch.Tensor) -> torch.Tensor:
        """Compute A-weighting curve as linear amplitude weights."""
        f2 = freqs ** 2
        # IEC 61672 formula
        num = (12194.0 ** 2) * (f2 ** 2)
        denom = (
            (f2 + 20.6 ** 2)
            * torch.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2))
            * (f2 + 12194.0 ** 2)
        )
        ra = torch.where(freqs > 0, num / (denom + 1e-12), torch.zeros_like(freqs))
        # Normalise so weight = 1.0 at 1 kHz
        ra_1khz = (12194.0 ** 2) * (1000.0 ** 4) / (
            (1000.0 ** 2 + 20.6 ** 2)
            * math.sqrt((1000.0 ** 2 + 107.7 ** 2) * (1000.0 ** 2 + 737.9 ** 2))
            * (1000.0 ** 2 + 12194.0 ** 2)
        )
        return ra / (ra_1khz + 1e-12)

    def forward(self, audio: torch.Tensor) -> torch.Tensor:
        """Compute per-frame A-weighted loudness.

        Args:
            audio: [B, T] float32 mono.

        Returns:
            loudness_db: [B, T_frames] float32 in dB.
        """
        B, T = audio.shape
        # STFT: window = hop_length * 4 for good frequency resolution
        window = torch.hann_window(self.n_fft, device=audio.device)
        stft = torch.stft(
            audio,
            n_fft=self.n_fft,
            hop_length=self.hop_length,
            win_length=self.n_fft,
            window=window,
            return_complex=True,
        )  # [B, n_fft//2+1, T_frames]

        # Power spectrum weighted by A-weighting
        power = stft.abs() ** 2  # [B, F, T_frames]
        weights = self.a_weights_linear.to(audio.device)  # [F]
        weights = rearrange(weights, "f -> 1 f 1")
        weighted_power = (power * weights).mean(dim=1)  # [B, T_frames]

        loudness_db = 10.0 * torch.log10(weighted_power.clamp(min=1e-10)) - self.ref_db
        return loudness_db


# ---------------------------------------------------------------------------
# Shared residual block for CNN encoders
# ---------------------------------------------------------------------------

class _ResBlock1d(nn.Module):
    """1D residual block with optional stride (for temporal downsampling)."""

    def __init__(self, channels: int, kernel_size: int = 5, stride: int = 1) -> None:
        super().__init__()
        pad = kernel_size // 2
        self.conv1 = nn.Conv1d(channels, channels, kernel_size, stride=stride, padding=pad)
        self.norm1 = nn.GroupNorm(min(8, channels), channels)
        self.conv2 = nn.Conv1d(channels, channels, kernel_size, padding=pad)
        self.norm2 = nn.GroupNorm(min(8, channels), channels)

        # Residual projection when stride > 1 to match temporal dimension
        if stride > 1:
            self.downsample = nn.Sequential(
                nn.Conv1d(channels, channels, 1, stride=stride),
                nn.GroupNorm(min(8, channels), channels),
            )
        else:
            self.downsample = nn.Identity()

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        residual = self.downsample(x)
        out = F.gelu(self.norm1(self.conv1(x)))
        out = self.norm2(self.conv2(out))
        return F.gelu(out + residual)


# ---------------------------------------------------------------------------
# Timbre Encoder
# ---------------------------------------------------------------------------

class TimbreEncoder(nn.Module):
    """Encode formant/timbre information as F0-invariant latent z_timbre.

    Architecture:
        log-mel [B, n_mels, T]
          → input projection Conv1d(n_mels, hidden_dim)
          → 6 residual blocks (blocks 3 and 5 use stride=2 for temporal downsampling)
          → output projection Conv1d(hidden_dim, timbre_dim)
          → z_timbre [B, timbre_dim, T_down]

    The Gradient Reversal Layer is exposed as ``self.grl`` so the adversarial
    branch (F0Predictor) can be attached externally.

    Args:
        n_mels:     Number of mel bins (default: 80).
        timbre_dim: Output latent dimension (default: 32).
        hidden_dim: Internal CNN channels (default: 256).
        grl_lambda: GRL reversal strength (default: 0.01).
    """

    def __init__(
        self,
        n_mels: int = 80,
        timbre_dim: int = 32,
        hidden_dim: int = 256,
        grl_lambda: float = 0.01,
    ) -> None:
        super().__init__()
        self.timbre_dim = timbre_dim

        self.input_proj = nn.Sequential(
            nn.Conv1d(n_mels, hidden_dim, kernel_size=1),
            nn.GroupNorm(min(8, hidden_dim), hidden_dim),
            nn.GELU(),
        )

        self.blocks = nn.Sequential(
            _ResBlock1d(hidden_dim, kernel_size=5, stride=1),   # block 0
            _ResBlock1d(hidden_dim, kernel_size=5, stride=1),   # block 1
            _ResBlock1d(hidden_dim, kernel_size=5, stride=2),   # block 2 — downsample ×2
            _ResBlock1d(hidden_dim, kernel_size=5, stride=1),   # block 3
            _ResBlock1d(hidden_dim, kernel_size=5, stride=2),   # block 4 — downsample ×2
            _ResBlock1d(hidden_dim, kernel_size=5, stride=1),   # block 5
        )

        self.output_proj = nn.Conv1d(hidden_dim, timbre_dim, kernel_size=1)

        # GRL for adversarial disentanglement (attach F0Predictor head externally)
        self.grl = GradientReversalLayer(lambda_=grl_lambda)

    def forward(self, log_mel: torch.Tensor) -> torch.Tensor:
        """Encode log-mel spectrogram to timbre latent.

        Args:
            log_mel: [B, n_mels, T] float32 log-mel spectrogram.

        Returns:
            z_timbre: [B, timbre_dim, T_down] — T_down = T // 4 due to two stride-2 blocks.
        """
        x = self.input_proj(log_mel)   # [B, hidden_dim, T]
        x = self.blocks(x)             # [B, hidden_dim, T_down]
        z = self.output_proj(x)        # [B, timbre_dim, T_down]
        return z

    def forward_with_grl(self, log_mel: torch.Tensor) -> torch.Tensor:
        """Forward pass with GRL applied — use this output for adversarial F0 prediction."""
        z = self.forward(log_mel)
        return self.grl(z)


# ---------------------------------------------------------------------------
# Noise Encoder
# ---------------------------------------------------------------------------

class NoiseEncoder(nn.Module):
    """Encode sibilant / breath noise as latent z_noise.

    Lighter architecture than TimbreEncoder (3 residual blocks).
    z_noise is NOT modified during pitch correction — sibilants are preserved.

    Args:
        n_mels:    Number of mel bins (default: 80).
        noise_dim: Output latent dimension (default: 16).
        hidden_dim: Internal CNN channels (default: 128).
    """

    def __init__(
        self,
        n_mels: int = 80,
        noise_dim: int = 16,
        hidden_dim: int = 128,
    ) -> None:
        super().__init__()
        self.noise_dim = noise_dim

        self.input_proj = nn.Sequential(
            nn.Conv1d(n_mels, hidden_dim, kernel_size=1),
            nn.GroupNorm(min(8, hidden_dim), hidden_dim),
            nn.GELU(),
        )

        self.blocks = nn.Sequential(
            _ResBlock1d(hidden_dim, kernel_size=5, stride=1),   # block 0
            _ResBlock1d(hidden_dim, kernel_size=5, stride=2),   # block 1 — downsample ×2
            _ResBlock1d(hidden_dim, kernel_size=5, stride=2),   # block 2 — downsample ×2
        )

        self.output_proj = nn.Conv1d(hidden_dim, noise_dim, kernel_size=1)

    def forward(self, log_mel: torch.Tensor) -> torch.Tensor:
        """Encode log-mel spectrogram to noise latent.

        Args:
            log_mel: [B, n_mels, T] float32 log-mel spectrogram.

        Returns:
            z_noise: [B, noise_dim, T_down] — T_down = T // 4.
        """
        x = self.input_proj(log_mel)   # [B, hidden_dim, T]
        x = self.blocks(x)             # [B, hidden_dim, T_down]
        z = self.output_proj(x)        # [B, noise_dim, T_down]
        return z
