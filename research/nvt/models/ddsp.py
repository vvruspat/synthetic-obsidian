"""
DDSP (Differentiable Digital Signal Processing) synthesis modules.

HarmonicSynth   — additive sinusoidal synthesis with anti-aliasing
NoiseSynth      — time-varying STFT filtered noise synthesis
AmplitudeDecoder — MLP: z_timbre → harmonic amplitudes with loudness conditioning
"""

from __future__ import annotations

import math
from typing import Optional

import torch
import torch.nn as nn
import torch.nn.functional as F
from einops import rearrange

_LOUDNESS_GAIN_OFFSET_DB = 20.0


# ---------------------------------------------------------------------------
# Utility helpers
# ---------------------------------------------------------------------------

def _upsample_to_audio(x: torch.Tensor, hop_length: int) -> torch.Tensor:
    """Linearly interpolate frame-rate tensor [B, C, T_frames] to audio rate [B, C, T_audio].

    Uses linear interpolation to avoid click artefacts at frame boundaries.
    """
    T_audio = x.shape[-1] * hop_length
    return F.interpolate(x, size=T_audio, mode="linear", align_corners=False)


# ---------------------------------------------------------------------------
# AmplitudeDecoder
# ---------------------------------------------------------------------------

class AmplitudeDecoder(nn.Module):
    """Predict per-harmonic amplitudes from timbre latent with loudness conditioning.

    The decoder maps z_timbre [B, timbre_dim, T] + loudness [B, 1, T] to a
    normalised amplitude envelope [B, n_harmonics, T], scaled by loudness.

    Args:
        timbre_dim:   Dimension of z_timbre (default: 32).
        n_harmonics:  Number of harmonic partials (default: 100).
        hidden_dim:   Hidden layer width (default: 256).
    """

    def __init__(
        self,
        timbre_dim: int = 32,
        n_harmonics: int = 100,
        hidden_dim: int = 256,
    ) -> None:
        super().__init__()
        self.n_harmonics = n_harmonics

        # Input: timbre_dim + 1 (loudness) → hidden → n_harmonics
        self.mlp = nn.Sequential(
            nn.Conv1d(timbre_dim + 1, hidden_dim, kernel_size=1),
            nn.GELU(),
            nn.Conv1d(hidden_dim, hidden_dim, kernel_size=1),
            nn.GELU(),
            nn.Conv1d(hidden_dim, n_harmonics, kernel_size=1),
        )

    def forward(
        self,
        z_timbre: torch.Tensor,
        loudness: torch.Tensor,
    ) -> torch.Tensor:
        """Decode harmonic amplitudes.

        Args:
            z_timbre: [B, timbre_dim, T_frames]
            loudness: [B, T_frames] or [B, 1, T_frames] in dB (normalised).

        Returns:
            harmonic_amps: [B, n_harmonics, T_frames] — positive, sum-normalised.
        """
        if loudness.dim() == 2:
            loudness = rearrange(loudness, "b t -> b 1 t")

        # Ensure temporal dimensions match (z_timbre may be at T//4, loudness at T)
        if loudness.shape[-1] != z_timbre.shape[-1]:
            loudness = F.interpolate(loudness, size=z_timbre.shape[-1], mode="linear", align_corners=False)

        x = torch.cat([z_timbre, loudness], dim=1)  # [B, timbre_dim+1, T]
        amps = self.mlp(x)                           # [B, n_harmonics, T]

        # Sigmoid → positive amplitudes; normalise so they sum to 1 per frame
        amps = torch.sigmoid(amps)
        amps = amps / (amps.sum(dim=1, keepdim=True) + 1e-8)

        # LoudnessEncoder reports an STFT power-derived feature whose typical
        # vocal range is around +20 dB. Offset it before converting to gain so
        # the harmonic sum stays near waveform scale instead of clipping.
        loudness_linear = 10.0 ** ((loudness - _LOUDNESS_GAIN_OFFSET_DB) / 20.0)
        amps = amps * loudness_linear

        return amps


# ---------------------------------------------------------------------------
# HarmonicSynth
# ---------------------------------------------------------------------------

class HarmonicSynth(nn.Module):
    """Differentiable additive harmonic synthesizer.

    Generates audio via cumulative-phase sinusoidal synthesis.  Each harmonic k
    oscillates at k * f0 with learnable amplitude from AmplitudeDecoder.  Harmonics
    above Nyquist are zeroed to prevent aliasing.

    Args:
        sample_rate:  Audio sample rate in Hz (default: 24000).
        hop_length:   Frames → samples ratio (default: 256).
        n_harmonics:  Maximum number of harmonic partials (default: 100).
        timbre_dim:   Dimension of z_timbre input to AmplitudeDecoder (default: 32).
    """

    def __init__(
        self,
        sample_rate: int = 24000,
        hop_length: int = 256,
        n_harmonics: int = 100,
        timbre_dim: int = 32,
    ) -> None:
        super().__init__()
        self.sample_rate = sample_rate
        self.hop_length = hop_length
        self.n_harmonics = n_harmonics
        self.nyquist = sample_rate / 2.0

        self.amplitude_decoder = AmplitudeDecoder(
            timbre_dim=timbre_dim,
            n_harmonics=n_harmonics,
        )

        # Harmonic indices: [1, 2, ..., n_harmonics]
        harmonic_idx = torch.arange(1, n_harmonics + 1, dtype=torch.float32)
        self.register_buffer("harmonic_idx", harmonic_idx)  # [n_harmonics]

    def forward(
        self,
        f0_hz: torch.Tensor,
        z_timbre: torch.Tensor,
        loudness: torch.Tensor,
    ) -> torch.Tensor:
        """Synthesise harmonic waveform.

        Args:
            f0_hz:    [B, T_frames] fundamental frequency in Hz (0 = unvoiced).
            z_timbre: [B, timbre_dim, T_frames_enc] timbre latent (may differ in T).
            loudness: [B, T_frames] A-weighted loudness in dB.

        Returns:
            waveform: [B, T_audio] where T_audio = T_frames * hop_length.
        """
        B, T_frames = f0_hz.shape

        # Decode harmonic amplitudes [B, n_harmonics, T_enc]
        harm_amps = self.amplitude_decoder(z_timbre, loudness)  # [B, n_harmonics, T_enc]

        # Upsample f0 to audio rate using its own frame count
        f0_audio = rearrange(f0_hz, "b t -> b 1 t")
        f0_audio = _upsample_to_audio(f0_audio, self.hop_length)  # [B, 1, T_audio_f0]
        T_audio = f0_audio.shape[-1]

        # Gate harmonic energy out of unvoiced regions. If f0 is zero but
        # amplitudes remain non-zero, the phase accumulator holds its previous
        # phase and the additive synth can emit DC-like steps/clicks.
        voiced_audio = rearrange((f0_hz > 0.0).float(), "b t -> b 1 t")
        voiced_audio = F.interpolate(
            voiced_audio, size=T_audio, mode="linear", align_corners=False
        )

        # Upsample harm_amps to the exact same T_audio (z_timbre may be at T//4)
        harm_amps_audio = F.interpolate(
            harm_amps, size=T_audio, mode="linear", align_corners=False
        )  # [B, n_harmonics, T_audio]
        harm_amps_audio = harm_amps_audio * voiced_audio

        # Harmonic frequencies: f0 * [1, 2, ..., n_harmonics]
        harmonic_idx = rearrange(self.harmonic_idx, "h -> 1 h 1")  # [1, n_harmonics, 1]
        harm_freqs = f0_audio * harmonic_idx                        # [B, n_harmonics, T_audio]

        # Anti-aliasing: zero amplitudes for harmonics above Nyquist
        above_nyquist = harm_freqs >= self.nyquist
        harm_amps_audio = harm_amps_audio.masked_fill(above_nyquist, 0.0)

        # Phase accumulation: instantaneous phase = cumsum(2π * f / fs)
        # Using cumsum is fully differentiable and avoids phase discontinuities
        phase_increments = 2.0 * math.pi * harm_freqs / self.sample_rate  # [B, n_harmonics, T_audio]
        phases = torch.cumsum(phase_increments, dim=-1)  # [B, n_harmonics, T_audio]

        # Synthesise and sum all harmonics
        sinusoids = torch.sin(phases)                       # [B, n_harmonics, T_audio]
        waveform = (harm_amps_audio * sinusoids).sum(dim=1)  # [B, T_audio]

        return waveform


# ---------------------------------------------------------------------------
# NoiseSynth
# ---------------------------------------------------------------------------

class _NoiseFilterDecoder(nn.Module):
    """MLP: z_noise [B, noise_dim, T] → spectral magnitudes [B, n_freqs, T].

    Outputs softplus-bounded magnitudes for frequency-domain noise shaping.
    n_freqs = n_fft // 2 + 1
    """

    def __init__(self, noise_dim: int = 16, n_freqs: int = 513, hidden_dim: int = 128) -> None:
        super().__init__()
        self.n_freqs = n_freqs
        self.mlp = nn.Sequential(
            nn.Conv1d(noise_dim, hidden_dim, kernel_size=1),
            nn.GELU(),
            nn.Conv1d(hidden_dim, hidden_dim, kernel_size=1),
            nn.GELU(),
            nn.Conv1d(hidden_dim, n_freqs, kernel_size=1),
        )

    def forward(self, z_noise: torch.Tensor) -> torch.Tensor:
        """Predict spectral magnitudes.

        Args:
            z_noise: [B, noise_dim, T_frames]

        Returns:
            magnitudes: [B, n_freqs, T_frames] — softplus-bounded non-negative values.
        """
        return F.softplus(self.mlp(z_noise))  # non-negative spectral magnitudes


class NoiseSynth(nn.Module):
    """Time-varying spectral noise synthesis via STFT frequency-domain filtering.

    Filters white noise in the frequency domain using per-frame spectral
    magnitudes predicted from z_noise. Uses STFT/ISTFT so overlap-add
    windowing is handled by torch — no clicks at frame boundaries.

    This matches the DDSP paper (Engel et al. 2020) filtered noise approach.

    Args:
        sample_rate:  Audio sample rate (default: 24000).
        hop_length:   STFT hop in samples (default: 256).
        n_fft:        FFT size (default: 1024). n_freqs = n_fft//2+1.
        noise_dim:    Dimension of z_noise (default: 16).
        filter_len:   Unused — kept for API compatibility.
    """

    def __init__(
        self,
        sample_rate: int = 24000,
        hop_length: int = 256,
        n_fft: int = 1024,
        noise_dim: int = 16,
        filter_len: int = 64,   # kept for backward compat, unused
    ) -> None:
        super().__init__()
        self.sample_rate = sample_rate
        self.hop_length = hop_length
        self.n_fft = n_fft
        self.n_freqs = n_fft // 2 + 1
        self.register_buffer("window", torch.hann_window(n_fft), persistent=False)

        self.filter_decoder = _NoiseFilterDecoder(
            noise_dim=noise_dim,
            n_freqs=self.n_freqs,
        )

    def forward(self, z_noise: torch.Tensor, target_length: Optional[int] = None) -> torch.Tensor:
        """Synthesise noise waveform via STFT frequency-domain filtering.

        No block boundaries → no clicks. ISTFT handles overlap-add with
        Hann windowing internally (matches DDSP paper, Engel et al. 2020).

        Args:
            z_noise:       [B, noise_dim, T_frames_enc].
            target_length: Desired output length in samples.

        Returns:
            waveform: [B, T_audio].
        """
        B = z_noise.shape[0]
        T_audio = target_length if target_length is not None else (
            z_noise.shape[-1] * self.hop_length
        )

        # 1. White noise source
        noise = torch.randn(B, T_audio, device=z_noise.device, dtype=z_noise.dtype)

        # 2. STFT → [B, n_freqs, T_stft]
        window = self.window.to(device=z_noise.device, dtype=z_noise.dtype)
        noise_stft = torch.stft(
            noise,
            n_fft=self.n_fft,
            hop_length=self.hop_length,
            win_length=self.n_fft,
            window=window,
            return_complex=True,
        )
        T_stft = noise_stft.shape[-1]

        # 3. Spectral magnitudes from encoder latent → upsample to T_stft
        magnitudes = self.filter_decoder(z_noise)          # [B, n_freqs, T_enc]
        magnitudes = F.interpolate(
            magnitudes, size=T_stft, mode="linear", align_corners=False
        )  # [B, n_freqs, T_stft]

        # 4. Frequency-domain shaping: multiply complex STFT by real magnitudes
        noise_filtered = noise_stft * magnitudes

        # 5. ISTFT → click-free waveform via overlap-add
        waveform = torch.istft(
            noise_filtered,
            n_fft=self.n_fft,
            hop_length=self.hop_length,
            win_length=self.n_fft,
            window=window,
            length=T_audio,
        )  # [B, T_audio]

        return waveform
