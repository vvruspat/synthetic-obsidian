"""
NeuralVocalTuner — full DDSP + Vocos pipeline for pitch correction.

Brings together:
  F0Encoder, LoudnessEncoder, TimbreEncoder, NoiseEncoder
  AmplitudeDecoder, HarmonicSynth, NoiseSynth
  Vocos vocoder (final mel → waveform)
"""

from __future__ import annotations

from typing import Dict, Optional

import torch
import torch.nn as nn
import torch.nn.functional as F
import torchaudio

from nvt.models.encoders import (
    F0Encoder,
    LoudnessEncoder,
    TimbreEncoder,
    NoiseEncoder,
)
from nvt.models.ddsp import HarmonicSynth, NoiseSynth

_LOUDNESS_TO_RMS_OFFSET_DB = 43.0
_LOUDNESS_HEADROOM_DB = 6.0
_SOFT_CLIP_THRESHOLD = 0.92


class NeuralVocalTuner(nn.Module):
    """Full neural vocal tuning pipeline.

    Analysis → Edit (pitch correction) → Synthesis

    Args:
        sample_rate:        Target sample rate in Hz (default: 24000).
        hop_length:         Analysis/synthesis hop in samples (default: 256).
        n_fft:              FFT size for mel spectrogram (default: 1024).
        n_mels:             Number of mel bins (default: 80).
        timbre_dim:         Timbre latent dimension (default: 32).
        noise_dim:          Noise latent dimension (default: 16).
        n_harmonics:        Number of DDSP harmonics (default: 100).
        noise_filter_len:   FIR filter length for NoiseSynth (default: 64).
        vocos_checkpoint:   HuggingFace model ID or local path for Vocos (default: "charactr/vocos-mel-24khz").
        freeze_vocos:       Whether to freeze Vocos weights (default: True).
        f0_confidence_threshold: Torchcrepe confidence threshold (default: 0.4).
    """

    def __init__(
        self,
        sample_rate: int = 24000,
        hop_length: int = 256,
        n_fft: int = 1024,
        n_mels: int = 80,
        timbre_dim: int = 32,
        noise_dim: int = 16,
        n_harmonics: int = 100,
        noise_filter_len: int = 64,
        vocos_checkpoint: str = "charactr/vocos-mel-24khz",
        freeze_vocos: bool = True,
        f0_confidence_threshold: float = 0.4,
    ) -> None:
        super().__init__()

        self.sample_rate = sample_rate
        self.hop_length = hop_length
        self.n_fft = n_fft
        self.n_mels = n_mels
        self.timbre_dim = timbre_dim
        self.noise_dim = noise_dim

        # --- Analysis encoders ---
        self.f0_encoder = F0Encoder(
            sample_rate=sample_rate,
            hop_length=hop_length,
            confidence_threshold=f0_confidence_threshold,
        )
        self.loudness_encoder = LoudnessEncoder(
            sample_rate=sample_rate,
            hop_length=hop_length,
            n_fft=n_fft,
        )
        self.timbre_encoder = TimbreEncoder(
            n_mels=n_mels,
            timbre_dim=timbre_dim,
        )
        self.noise_encoder = NoiseEncoder(
            n_mels=n_mels,
            noise_dim=noise_dim,
        )

        # --- Mel spectrogram transform (shared, non-trainable) ---
        self.mel_transform = torchaudio.transforms.MelSpectrogram(
            sample_rate=sample_rate,
            n_fft=n_fft,
            hop_length=hop_length,
            n_mels=n_mels,
            f_min=20.0,
            f_max=sample_rate / 2.0,
        )

        # --- DDSP synthesizers ---
        self.harmonic_synth = HarmonicSynth(
            sample_rate=sample_rate,
            hop_length=hop_length,
            n_harmonics=n_harmonics,
            timbre_dim=timbre_dim,
        )
        self.noise_synth = NoiseSynth(
            sample_rate=sample_rate,
            hop_length=hop_length,
            n_fft=n_fft,
            noise_dim=noise_dim,
            filter_len=noise_filter_len,
        )

        # --- Vocos vocoder ---
        self.vocos: Optional[nn.Module] = None
        self._vocos_checkpoint = vocos_checkpoint
        self._freeze_vocos = freeze_vocos
        self._load_vocos(vocos_checkpoint, freeze_vocos)

    def _load_vocos(self, checkpoint: str, freeze: bool) -> None:
        """Attempt to load Vocos. Silently skips if not installed."""
        try:
            from vocos import Vocos  # type: ignore
            self.vocos = Vocos.from_pretrained(checkpoint)
            if freeze:
                for p in self.vocos.parameters():
                    p.requires_grad_(False)
        except Exception:
            # Vocos not installed or checkpoint unavailable — DDSP output used directly
            self.vocos = None

    # ------------------------------------------------------------------
    # Mel helper
    # ------------------------------------------------------------------

    def _compute_log_mel(self, audio: torch.Tensor) -> torch.Tensor:
        """Compute log-mel spectrogram.

        Args:
            audio: [B, T] float32 waveform.

        Returns:
            log_mel: [B, n_mels, T_frames] float32.
        """
        mel = self.mel_transform(audio)  # [B, n_mels, T_frames]
        log_mel = torch.log(mel.clamp(min=1e-5))
        return log_mel

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def encode(self, audio: torch.Tensor, sample_rate: Optional[int] = None) -> Dict[str, torch.Tensor]:
        """Analysis stage: extract all latents from raw audio.

        Args:
            audio:       [B, T] float32 mono waveform.
            sample_rate: If provided and differs from self.sample_rate, audio is resampled.

        Returns:
            dict with keys:
                f0_hz     [B, T_frames]
                voiced    [B, T_frames] bool
                loudness  [B, T_frames]
                z_timbre  [B, timbre_dim, T_down]
                z_noise   [B, noise_dim, T_down]
                log_mel   [B, n_mels, T_frames]
        """
        if sample_rate is not None and sample_rate != self.sample_rate:
            audio = torchaudio.functional.resample(audio, sample_rate, self.sample_rate)

        # F0 and voicing
        with torch.no_grad():
            f0_hz, voiced = self.f0_encoder(audio)

        # Loudness
        loudness = self.loudness_encoder(audio)

        # Log-mel for CNN encoders
        log_mel = self._compute_log_mel(audio)

        # Timbre and noise latents
        z_timbre = self.timbre_encoder(log_mel)
        z_noise = self.noise_encoder(log_mel)

        return {
            "f0_hz": f0_hz,
            "voiced": voiced,
            "loudness": loudness,
            "z_timbre": z_timbre,
            "z_noise": z_noise,
            "log_mel": log_mel,
        }

    def decode(
        self,
        f0_hz: torch.Tensor,
        loudness: torch.Tensor,
        z_timbre: torch.Tensor,
        z_noise: torch.Tensor,
    ) -> torch.Tensor:
        """Synthesis stage: generate waveform from latents.

        Args:
            f0_hz:    [B, T_frames] fundamental frequency (Hz).
            loudness: [B, T_frames] A-weighted loudness (dB).
            z_timbre: [B, timbre_dim, T_down].
            z_noise:  [B, noise_dim, T_down].

        Returns:
            waveform: [B, T_audio] float32.
        """
        # DDSP harmonic synthesis
        harmonic_audio = self.harmonic_synth(f0_hz, z_timbre, loudness)  # [B, T_audio]

        # DDSP noise synthesis — match harmonic length exactly
        # (z_noise is 4× downsampled by encoder; target_length compensates)
        noise_audio = self.noise_synth(z_noise, target_length=harmonic_audio.shape[-1])

        # Align lengths (harmonic and noise may differ by a few samples)
        min_len = min(harmonic_audio.shape[-1], noise_audio.shape[-1])
        ddsp_audio = harmonic_audio[:, :min_len] + noise_audio[:, :min_len]
        ddsp_audio = self._apply_output_guardrails(ddsp_audio, loudness)

        if self.vocos is None or self.training:
            # During training: return raw DDSP audio so gradients flow back through
            # our encoders/decoders.  Vocos is applied only at inference time.
            return ddsp_audio

        # ── Inference: pass DDSP output through Vocos neural vocoder ──────────
        # Correct API: feature_extractor(audio) → Vocos features → decode() → waveform.
        # Vocos has its own MelSpectrogram (n_mels=100, power=1, 24 kHz) — never pass
        # our log-mel directly.
        try:
            with torch.no_grad():
                features = self.vocos.feature_extractor(ddsp_audio)  # [B, 100, T_v]
                waveform = self.vocos.decode(features)                # [B, T_out]
        except Exception:
            waveform = ddsp_audio

        # Align to DDSP output length
        target_len = ddsp_audio.shape[-1]
        if waveform.shape[-1] > target_len:
            waveform = waveform[:, :target_len]
        elif waveform.shape[-1] < target_len:
            waveform = F.pad(waveform, (0, target_len - waveform.shape[-1]))

        return waveform

    @staticmethod
    @torch.no_grad()
    def stabilize_f0_contour(
        f0_hz: torch.Tensor,
        max_gap_frames: int = 2,
        median_width: int = 5,
        max_deviation_semitones: float = 7.0,
    ) -> torch.Tensor:
        """Clean obvious F0 dropouts/spikes before DDSP synthesis.

        This is an inference-side guard for torchcrepe contours. It fills only
        tiny unvoiced gaps between voiced regions and replaces isolated pitch
        spikes with a local median in log-frequency space. Longer unvoiced
        regions are preserved, so breaths and consonants still mute the
        harmonic synth instead of turning into pitched artifacts.
        """
        if f0_hz.dim() != 2:
            raise ValueError("f0_hz must have shape [B, T_frames]")

        if median_width < 1 or median_width % 2 == 0:
            raise ValueError("median_width must be a positive odd integer")

        cleaned = f0_hz.clone()
        eps = 1e-6
        threshold_octaves = max_deviation_semitones / 12.0
        half_width = median_width // 2

        for b in range(cleaned.shape[0]):
            contour = cleaned[b]
            voiced = contour > 0.0
            voiced_indices = torch.nonzero(voiced, as_tuple=False).flatten()
            if voiced_indices.numel() < 2:
                continue

            # Fill short zero runs bounded by voiced frames. These are usually
            # detector dropouts rather than real breaths.
            for left, right in zip(voiced_indices[:-1], voiced_indices[1:]):
                left_i = int(left.item())
                right_i = int(right.item())
                gap = right_i - left_i - 1
                if 0 < gap <= max_gap_frames:
                    start = torch.log2(contour[left_i].clamp_min(eps))
                    end = torch.log2(contour[right_i].clamp_min(eps))
                    interp = torch.linspace(
                        0.0,
                        1.0,
                        gap + 2,
                        device=contour.device,
                        dtype=contour.dtype,
                    )[1:-1]
                    contour[left_i + 1 : right_i] = 2.0 ** (start + (end - start) * interp)

            voiced = contour > 0.0
            voiced_indices = torch.nonzero(voiced, as_tuple=False).flatten()
            if voiced_indices.numel() < median_width:
                continue

            log_f0 = torch.log2(contour.clamp_min(eps))
            smoothed = contour.clone()
            for idx in voiced_indices:
                i = int(idx.item())
                start = max(0, i - half_width)
                end = min(contour.shape[0], i + half_width + 1)
                local = log_f0[start:end][voiced[start:end]]
                if local.numel() < 3:
                    continue
                median = local.median()
                if torch.abs(log_f0[i] - median) > threshold_octaves:
                    smoothed[i] = 2.0 ** median

            cleaned[b] = smoothed

        return cleaned

    def _apply_output_guardrails(self, audio: torch.Tensor, loudness: torch.Tensor) -> torch.Tensor:
        """Keep DDSP output in range without flattening natural dynamics.

        LoudnessEncoder's feature is power-derived and sits around +20 dB for
        -23 dBFS vocal RMS, so this calibration maps it back to waveform scale.
        We only attenuate when RMS exceeds the source-derived level plus a
        little headroom; normal dynamics are left alone. A light tanh soft clip
        catches isolated peaks that otherwise become audible clicks.
        """
        if loudness.dim() == 3:
            loudness = loudness.squeeze(1)

        eps = 1e-7
        target_rms = 10.0 ** ((loudness.mean(dim=-1, keepdim=True) - _LOUDNESS_TO_RMS_OFFSET_DB) / 20.0)
        max_rms = target_rms * (10.0 ** (_LOUDNESS_HEADROOM_DB / 20.0))
        current_rms = torch.sqrt(audio.pow(2).mean(dim=-1, keepdim=True) + eps)
        gain = torch.minimum(torch.ones_like(current_rms), max_rms / (current_rms + eps)).detach()
        audio = audio * gain

        threshold = _SOFT_CLIP_THRESHOLD
        return threshold * torch.tanh(audio / threshold)

    def forward(
        self,
        audio: torch.Tensor,
        f0_target: Optional[torch.Tensor] = None,
        log_mel: Optional[torch.Tensor] = None,
        loudness: Optional[torch.Tensor] = None,
    ) -> Dict[str, torch.Tensor]:
        """Full forward pass for training.

        Args:
            audio:     [B, T] float32 input waveform at self.sample_rate.
            f0_target: [B, T_frames] F0 in Hz (pre-computed or from piano roll).
                       When provided, torchcrepe F0 inference is skipped entirely —
                       this is the fast path used during training (5–10× faster).
            log_mel:   [B, n_mels, T_frames] pre-computed log-mel (optional).
                       When None, computed from audio on-the-fly.
            loudness:  [B, T_frames] pre-computed A-weighted loudness (optional).
                       When None, computed from audio on-the-fly.

        Returns:
            dict with:
                waveform    [B, T_audio] reconstructed audio
                f0_hz       [B, T_frames] F0 used for synthesis
                voiced      [B, T_frames] voicing flags (bool)
                loudness    [B, T_frames] loudness features
                z_timbre    [B, timbre_dim, T_down]
                z_noise     [B, noise_dim, T_down]
        """
        # ── Log-mel (shared by both CNN encoders) ─────────────────────────────
        if log_mel is None:
            log_mel = self._compute_log_mel(audio)

        # ── Loudness ──────────────────────────────────────────────────────────
        if loudness is None:
            loudness = self.loudness_encoder(audio)

        # ── F0 — skip torchcrepe if f0_target is already provided ─────────────
        if f0_target is not None:
            f0_hz = f0_target
            # voiced = frames where F0 > 0
            voiced = f0_hz > 0.0
        else:
            with torch.no_grad():
                f0_hz, voiced = self.f0_encoder(audio)

        # ── CNN encoders (timbre + noise) ──────────────────────────────────────
        z_timbre = self.timbre_encoder(log_mel)
        z_noise  = self.noise_encoder(log_mel)

        # ── Align F0 frames with loudness if they differ ───────────────────────
        if f0_hz.shape[-1] != loudness.shape[-1]:
            f0_hz = F.interpolate(
                f0_hz.unsqueeze(1).float(),
                size=loudness.shape[-1],
                mode="linear",
                align_corners=False,
            ).squeeze(1)
            voiced = f0_hz > 0.0

        waveform = self.decode(
            f0_hz=f0_hz,
            loudness=loudness,
            z_timbre=z_timbre,
            z_noise=z_noise,
        )

        return {
            "waveform": waveform,
            "f0_hz": f0_hz,
            "voiced": voiced,
            "loudness": loudness,
            "z_timbre": z_timbre,
            "z_noise": z_noise,
        }

    @torch.no_grad()
    def correct_pitch(
        self,
        audio: torch.Tensor,
        f0_target: torch.Tensor,
        sample_rate: Optional[int] = None,
    ) -> torch.Tensor:
        """Convenience method for inference: apply pitch correction.

        Args:
            audio:       [B, T] float32 input waveform.
            f0_target:   [B, T_frames] corrected F0 in Hz from piano roll / note grid.
            sample_rate: Input sample rate; resampled if differs from model rate.

        Returns:
            corrected_audio: [B, T_audio] float32 at self.sample_rate.
        """
        if sample_rate is not None and sample_rate != self.sample_rate:
            audio = torchaudio.functional.resample(audio, sample_rate, self.sample_rate)

        result = self.forward(audio, f0_target=f0_target)
        return result["waveform"]
