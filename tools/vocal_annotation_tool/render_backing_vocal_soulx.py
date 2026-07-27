#!/usr/bin/env python3
"""Neurally render backing vocals with SoulX-Singer-SVC.

The lead waveform supplies linguistic timing and singer identity. Generated
backing-vocal notes replace its F0 contour before SoulX-Singer resynthesizes the
waveform. This module is offline-only and is intended to be kept alive by
``backing_audio_worker.py`` so the large checkpoint is loaded once.
"""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from pathlib import Path
from typing import Any

os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")

import numpy as np
import pyworld as pw
import soundfile as sf
import torch
from torch import nn
from scipy.ndimage import gaussian_filter1d, median_filter
from scipy.signal import resample_poly

from soulx_lora import load_adapter, remove_lora


SOULX_SAMPLE_RATE = 24_000
SOULX_HOP_SIZE = 480
SOULX_F0_RATE = SOULX_SAMPLE_RATE // SOULX_HOP_SIZE
DEFAULT_RUNTIME_ROOT = Path.home() / ".synthetic_obsidian" / "runtime" / "soulx-singer"
DEFAULT_SOURCE_ROOT = DEFAULT_RUNTIME_ROOT / "source"
DEFAULT_MODEL_PATH = Path.home() / ".synthetic_obsidian" / "models" / "SoulX-Singer" / "model-svc.pt"
DEFAULT_CONFIG_PATH = DEFAULT_SOURCE_ROOT / "soulxsinger" / "config" / "soulxsinger.yaml"


class CpuVocoderBridge(nn.Module):
    """Run Vocos on CPU because its complex ISTFT is not supported by MPS."""

    def __init__(self, vocoder: nn.Module) -> None:
        super().__init__()
        self.vocoder = vocoder.to("cpu")

    def forward(self, mel: torch.Tensor) -> torch.Tensor:
        source_device = mel.device
        audio = self.vocoder(mel.to("cpu"))
        return audio.to(source_device)


def choose_device(requested: str = "auto") -> str:
    if requested != "auto":
        return requested
    if torch.backends.mps.is_available():
        return "mps"
    if torch.cuda.is_available():
        return "cuda"
    return "cpu"


def midi_to_hz(midi: np.ndarray | float) -> np.ndarray | float:
    return 440.0 * np.power(2.0, (np.asarray(midi) - 69.0) / 12.0)


def hz_to_midi(hz: np.ndarray) -> np.ndarray:
    return 69.0 + 12.0 * np.log2(np.maximum(hz, 1e-8) / 440.0)


def load_mono_24k(path: Path) -> np.ndarray:
    audio, sample_rate = sf.read(str(path), dtype="float32", always_2d=True)
    mono = np.mean(audio, axis=1, dtype=np.float32)
    if sample_rate != SOULX_SAMPLE_RATE:
        divisor = math.gcd(int(sample_rate), SOULX_SAMPLE_RATE)
        mono = resample_poly(
            mono,
            SOULX_SAMPLE_RATE // divisor,
            int(sample_rate) // divisor,
        ).astype(np.float32)
    return np.ascontiguousarray(mono)


def extract_f0(audio: np.ndarray) -> np.ndarray:
    frame_period_ms = 1000.0 / SOULX_F0_RATE
    source = audio.astype(np.float64, copy=False)
    f0, time_axis = pw.dio(
        source,
        SOULX_SAMPLE_RATE,
        frame_period=frame_period_ms,
        f0_floor=32.0,
        f0_ceil=1_200.0,
    )
    f0 = pw.stonemask(source, f0, time_axis, SOULX_SAMPLE_RATE)
    expected_frames = max(1, int(math.ceil(len(audio) / SOULX_HOP_SIZE)))
    if len(f0) != expected_frames:
        source_times = np.arange(len(f0), dtype=np.float64) / SOULX_F0_RATE
        target_times = np.arange(expected_frames, dtype=np.float64) / SOULX_F0_RATE
        voiced = f0 > 0.0
        if np.any(voiced):
            interpolated = np.interp(target_times, source_times[voiced], f0[voiced])
            uv = np.interp(target_times, source_times, voiced.astype(np.float64)) < 0.5
            interpolated[uv] = 0.0
            f0 = interpolated
        else:
            f0 = np.zeros(expected_frames, dtype=np.float64)
    return f0.astype(np.float32)


def note_pitch_at(note: dict[str, Any], time_sec: float) -> float:
    curve = note.get("curve")
    if not isinstance(curve, list) or not curve:
        return float(note.get("pitch_exact", note.get("pitch", 60.0)))

    points: list[tuple[float, float]] = []
    for value in curve:
        if not isinstance(value, dict):
            continue
        try:
            points.append((float(value["time"]), float(value["midi"])))
        except (KeyError, TypeError, ValueError):
            continue
    if not points:
        return float(note.get("pitch_exact", note.get("pitch", 60.0)))

    points.sort(key=lambda item: item[0])
    if time_sec <= points[0][0]:
        return points[0][1]
    if time_sec >= points[-1][0]:
        return points[-1][1]
    for (left_time, left_midi), (right_time, right_midi) in zip(points, points[1:]):
        if left_time <= time_sec <= right_time:
            alpha = (time_sec - left_time) / max(1e-6, right_time - left_time)
            return left_midi + (right_midi - left_midi) * alpha
    return points[-1][1]


def build_target_f0(
    notes: list[dict[str, Any]],
    source_f0: np.ndarray,
) -> tuple[np.ndarray, np.ndarray]:
    """Build a 50 Hz F0 contour while retaining only local vocal expression."""
    target_f0 = np.zeros_like(source_f0, dtype=np.float32)
    note_mask = np.zeros_like(source_f0, dtype=np.float32)
    frame_times = np.arange(len(source_f0), dtype=np.float64) / SOULX_F0_RATE

    previous_note: dict[str, Any] | None = None
    for note in sorted(notes, key=lambda item: (float(item.get("start", 0.0)), float(item.get("end", 0.0)))):
        start = float(note.get("voiced_start", note.get("start", 0.0)))
        end = float(note.get("voiced_end", note.get("end", start)))
        if end <= start:
            continue

        active = (frame_times >= start) & (frame_times < end)
        voiced = active & (source_f0 > 32.0)
        if not np.any(voiced):
            continue

        indices = np.flatnonzero(active)
        base_midi = np.asarray(
            [note_pitch_at(note, float(frame_times[index])) for index in indices],
            dtype=np.float32,
        )

        if previous_note is not None:
            previous_end = float(previous_note.get("voiced_end", previous_note.get("end", start)))
            same_syllable = bool(note.get("syllable_id")) and (
                str(note.get("syllable_id")) == str(previous_note.get("syllable_id"))
            )
            is_legato = bool(note.get("legato_from_previous")) or same_syllable
            gap = start - previous_end
            if is_legato and -0.02 <= gap <= 0.08:
                transition_seconds = min(0.08, max(0.03, (end - start) * 0.25))
                alpha = np.clip((frame_times[indices] - start) / transition_seconds, 0.0, 1.0).astype(np.float32)
                alpha = alpha * alpha * (3.0 - 2.0 * alpha)
                previous_midi = note_pitch_at(previous_note, min(start, previous_end))
                base_midi = previous_midi + (base_midi - previous_midi) * alpha

        source_region = source_f0[indices]
        voiced_region = source_region > 32.0
        source_midi = np.zeros_like(source_region, dtype=np.float32)
        source_midi[voiced_region] = hz_to_midi(source_region[voiced_region])
        if np.count_nonzero(voiced_region) >= 2:
            positions = np.arange(len(indices), dtype=np.float32)
            filled = np.interp(
                positions,
                positions[voiced_region],
                source_midi[voiced_region],
            ).astype(np.float32)
            local_center = median_filter(filled, size=15, mode="nearest")
            expression = np.clip(filled - local_center, -0.35, 0.35)
        else:
            expression = np.zeros_like(source_region, dtype=np.float32)

        target_midi = base_midi + expression
        target_region = np.asarray(midi_to_hz(target_midi), dtype=np.float32)
        target_region[~voiced_region] = 0.0
        target_f0[indices] = target_region
        note_mask[indices] = 1.0
        previous_note = note

    return target_f0, note_mask


def select_prompt_window(
    audio: np.ndarray,
    f0: np.ndarray,
    duration_sec: float = 8.0,
) -> tuple[np.ndarray, np.ndarray, float]:
    window_frames = min(len(f0), max(1, int(round(duration_sec * SOULX_F0_RATE))))
    if len(f0) <= window_frames:
        return audio, f0, 0.0

    voiced = (f0 > 32.0).astype(np.float32)
    kernel = np.ones(window_frames, dtype=np.float32)
    scores = np.convolve(voiced, kernel, mode="valid")
    start_frame = int(np.argmax(scores))
    end_frame = start_frame + window_frames
    start_sample = start_frame * SOULX_HOP_SIZE
    end_sample = min(len(audio), end_frame * SOULX_HOP_SIZE)
    return audio[start_sample:end_sample], f0[start_frame:end_frame], start_frame / SOULX_F0_RATE


def select_best_prompt(
    request: dict[str, Any],
    target_audio_path: Path,
    target_audio: np.ndarray,
    target_f0: np.ndarray,
) -> tuple[np.ndarray, np.ndarray, float, Path]:
    """Pick the most consistently voiced prompt from the active local profile."""
    candidates: list[Path] = []
    raw_candidates = request.get("prompt_audio_candidates", [])
    if isinstance(raw_candidates, list):
        for value in raw_candidates:
            if isinstance(value, str) and value:
                candidates.append(Path(value).expanduser().resolve())
    candidates.append(target_audio_path)

    best: tuple[np.ndarray, np.ndarray, float, Path] | None = None
    best_score = -1.0
    for candidate_path in candidates:
        try:
            if candidate_path == target_audio_path:
                candidate_audio = target_audio
                candidate_f0 = target_f0
            else:
                candidate_audio = load_mono_24k(candidate_path)
                if not len(candidate_audio):
                    continue
                candidate_f0 = extract_f0(candidate_audio)
            prompt_audio, prompt_f0, prompt_start = select_prompt_window(
                candidate_audio,
                candidate_f0,
            )
        except (OSError, RuntimeError, ValueError):
            continue

        voiced_ratio = float(np.mean(prompt_f0 > 32.0)) if len(prompt_f0) else 0.0
        duration_score = min(1.0, len(prompt_audio) / (8.0 * SOULX_SAMPLE_RATE))
        score = voiced_ratio * 0.9 + duration_score * 0.1
        if score > best_score:
            best_score = score
            best = prompt_audio, prompt_f0, prompt_start, candidate_path

    if best is None:
        raise ValueError("no usable voice prompt was found")
    return best


def build_sample_mask(note_mask: np.ndarray, sample_count: int) -> np.ndarray:
    expanded = np.repeat(note_mask, SOULX_HOP_SIZE)[:sample_count]
    if len(expanded) < sample_count:
        expanded = np.pad(expanded, (0, sample_count - len(expanded)))
    # Include unvoiced consonants around the voiced body, then use a soft edge.
    radius = int(round(0.10 * SOULX_SAMPLE_RATE))
    if radius > 0 and np.any(expanded > 0.0):
        from scipy.ndimage import maximum_filter1d

        expanded = maximum_filter1d(
            expanded,
            size=radius * 2 + 1,
            mode="constant",
        ) > 0.0
    sigma = max(1.0, 0.012 * SOULX_SAMPLE_RATE)
    return np.clip(gaussian_filter1d(expanded.astype(np.float32), sigma=sigma), 0.0, 1.0)


class SoulXRenderer:
    def __init__(
        self,
        source_root: Path = DEFAULT_SOURCE_ROOT,
        model_path: Path = DEFAULT_MODEL_PATH,
        config_path: Path = DEFAULT_CONFIG_PATH,
        device: str = "auto",
        steps: int = 16,
        cfg: float = 3.0,
    ) -> None:
        self.source_root = source_root.expanduser().resolve()
        self.model_path = model_path.expanduser().resolve()
        self.config_path = config_path.expanduser().resolve()
        self.device = choose_device(device)
        self.steps = steps
        self.cfg = cfg

        if not self.source_root.is_dir():
            raise FileNotFoundError(f"SoulX-Singer source not found: {self.source_root}")
        if not self.model_path.is_file():
            raise FileNotFoundError(f"SoulX-Singer SVC checkpoint not found: {self.model_path}")
        if not self.config_path.is_file():
            raise FileNotFoundError(f"SoulX-Singer config not found: {self.config_path}")

        sys.path.insert(0, str(self.source_root))
        from cli.inference_svc import build_model  # pylint: disable=import-outside-toplevel
        from soulxsinger.utils.file_utils import load_config  # pylint: disable=import-outside-toplevel

        self.config = load_config(str(self.config_path))
        self.model = build_model(
            model_path=str(self.model_path),
            config=self.config,
            device=self.device,
            use_fp16=False,
        )
        if self.device == "mps":
            self.model.vocoder = CpuVocoderBridge(self.model.vocoder)
        self.active_adapter_path: Path | None = None

    def activate_adapter(self, requested_path: str | None) -> None:
        adapter_path = (
            Path(requested_path).expanduser().resolve()
            if requested_path
            else None
        )
        if adapter_path == self.active_adapter_path:
            return
        if adapter_path is None:
            remove_lora(self.model)
        else:
            if not adapter_path.is_file():
                raise FileNotFoundError(f"SoulX LoRA adapter not found: {adapter_path}")
            load_adapter(self.model, adapter_path, device=self.device)
        self.model.eval()
        self.active_adapter_path = adapter_path

    def render(self, request: dict[str, Any]) -> dict[str, Any]:
        self.activate_adapter(request.get("voice_adapter"))
        audio_path = Path(request["audio"]).expanduser().resolve()
        output_path = Path(request["output"]).expanduser().resolve()
        notes = request.get("backing_notes", [])
        if not isinstance(notes, list) or not notes:
            raise ValueError("request has no backing_notes")

        audio = load_mono_24k(audio_path)
        if not len(audio):
            raise ValueError(f"empty audio file: {audio_path}")

        source_f0 = extract_f0(audio)
        target_f0, note_mask = build_target_f0(notes, source_f0)
        if np.count_nonzero(target_f0) < 5:
            raise ValueError("backing notes do not overlap voiced source audio")

        prompt_audio, prompt_f0, prompt_start, prompt_source = select_best_prompt(
            request,
            audio_path,
            audio,
            source_f0,
        )
        prompt_wav = torch.from_numpy(prompt_audio).unsqueeze(0).to(self.device)
        target_wav = torch.from_numpy(audio).unsqueeze(0).to(self.device)
        prompt_f0_tensor = torch.from_numpy(prompt_f0).unsqueeze(0).to(self.device)
        target_f0_tensor = torch.from_numpy(target_f0).unsqueeze(0).to(self.device)

        with torch.inference_mode():
            generated, _ = self.model.infer(
                pt_wav=prompt_wav,
                gt_wav=target_wav,
                pt_f0=prompt_f0_tensor,
                gt_f0=target_f0_tensor,
                auto_shift=False,
                pitch_shift=0,
                n_steps=int(request.get("steps", self.steps)),
                cfg=float(request.get("cfg", self.cfg)),
                use_fp16=False,
            )

        rendered = generated.squeeze().float().cpu().numpy()
        if len(rendered) < len(audio):
            rendered = np.pad(rendered, (0, len(audio) - len(rendered)))
        rendered = rendered[: len(audio)]
        rendered *= build_sample_mask(note_mask, len(rendered))

        peak = float(np.max(np.abs(rendered))) if len(rendered) else 0.0
        if peak > 0.98:
            rendered *= 0.98 / peak

        output_path.parent.mkdir(parents=True, exist_ok=True)
        sf.write(str(output_path), rendered.astype(np.float32), SOULX_SAMPLE_RATE)
        return {
            "output": str(output_path),
            "sample_rate": SOULX_SAMPLE_RATE,
            "duration": len(rendered) / SOULX_SAMPLE_RATE,
            "notes": len(notes),
            "engine": "SoulX-Singer-SVC",
            "device": self.device,
            "steps": int(request.get("steps", self.steps)),
            "prompt_start": prompt_start,
            "prompt_duration": len(prompt_audio) / SOULX_SAMPLE_RATE,
            "prompt_source": str(prompt_source),
            "voice_adapter": (
                str(self.active_adapter_path)
                if self.active_adapter_path is not None
                else ""
            ),
        }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("request_json", type=Path)
    parser.add_argument("--source-root", type=Path, default=DEFAULT_SOURCE_ROOT)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL_PATH)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG_PATH)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--steps", type=int, default=16)
    args = parser.parse_args()

    renderer = SoulXRenderer(
        source_root=args.source_root,
        model_path=args.model,
        config_path=args.config,
        device=args.device,
        steps=args.steps,
    )
    request = json.loads(args.request_json.read_text(encoding="utf-8"))
    print(json.dumps(renderer.render(request), ensure_ascii=False))


if __name__ == "__main__":
    main()
