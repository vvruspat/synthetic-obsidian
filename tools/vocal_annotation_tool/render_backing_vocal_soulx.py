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
from typing import Any, Callable

os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")

import numpy as np
import pyworld as pw
import soundfile as sf
import torch
from torch import nn
from scipy.ndimage import gaussian_filter1d
from scipy.signal import resample_poly

from soulx_lora import load_adapter, remove_lora
from vocal_expression import (
    analyze_expression_profile,
    build_target_f0,
    default_expression_profile,
    load_expression_profile_for_adapter,
)


SOULX_SAMPLE_RATE = 24_000
SOULX_HOP_SIZE = 480
SOULX_F0_RATE = SOULX_SAMPLE_RATE // SOULX_HOP_SIZE
DEFAULT_INFERENCE_STEPS = 32
DEFAULT_INFERENCE_CFG = 3.0
DEFAULT_LORA_CFG = 2.0
DEFAULT_RUNTIME_ROOT = Path.home() / ".synthetic_obsidian" / "runtime" / "soulx-singer"
DEFAULT_SOURCE_ROOT = DEFAULT_RUNTIME_ROOT / "source"
DEFAULT_MODEL_PATH = Path.home() / ".synthetic_obsidian" / "models" / "SoulX-Singer" / "model-svc.pt"
DEFAULT_CONFIG_PATH = DEFAULT_SOURCE_ROOT / "soulxsinger" / "config" / "soulxsinger.yaml"
DEFAULT_RMVPE_MODEL_PATH = Path.home() / ".synthetic_obsidian" / "models" / "rmvpe" / "rmvpe.pt"


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


def select_prompt_window(
    audio: np.ndarray,
    f0: np.ndarray,
    duration_sec: float = 8.0,
    target_register_hz: float | None = None,
) -> tuple[np.ndarray, np.ndarray, float]:
    window_frames = min(len(f0), max(1, int(round(duration_sec * SOULX_F0_RATE))))
    if len(f0) <= window_frames:
        return audio, f0, 0.0

    voiced = (f0 > 32.0).astype(np.float32)
    kernel = np.ones(window_frames, dtype=np.float32)
    voiced_counts = np.convolve(voiced, kernel, mode="valid")
    candidate_stride = max(1, int(round(0.5 * SOULX_F0_RATE)))
    candidate_starts = set(range(0, len(voiced_counts), candidate_stride))
    candidate_starts.add(int(np.argmax(voiced_counts)))
    candidate_starts.add(len(voiced_counts) - 1)

    start_frame = 0
    best_score = -1.0
    for candidate_start in sorted(candidate_starts):
        candidate_f0 = f0[candidate_start:candidate_start + window_frames]
        candidate_voiced = candidate_f0[candidate_f0 > 32.0]
        voiced_ratio = len(candidate_voiced) / max(1, len(candidate_f0))
        score = voiced_ratio
        if target_register_hz is not None and target_register_hz > 32.0 and len(candidate_voiced):
            candidate_register_hz = float(np.median(candidate_voiced))
            distance_cents = abs(
                1_200.0 * math.log2(candidate_register_hz / target_register_hz)
            )
            register_score = math.exp(-0.5 * (distance_cents / 350.0) ** 2)
            score = 0.70 * voiced_ratio + 0.30 * register_score
        if score > best_score:
            best_score = score
            start_frame = candidate_start

    end_frame = start_frame + window_frames
    start_sample = start_frame * SOULX_HOP_SIZE
    end_sample = min(len(audio), end_frame * SOULX_HOP_SIZE)
    return audio[start_sample:end_sample], f0[start_frame:end_frame], start_frame / SOULX_F0_RATE


def prompt_score(
    prompt_f0: np.ndarray,
    target_register_hz: float | None,
) -> float:
    voiced = prompt_f0[prompt_f0 > 32.0]
    voiced_ratio = len(voiced) / max(1, len(prompt_f0))
    if target_register_hz is None or target_register_hz <= 32.0 or not len(voiced):
        return voiced_ratio

    prompt_register_hz = float(np.median(voiced))
    distance_cents = abs(
        1_200.0 * math.log2(prompt_register_hz / target_register_hz)
    )
    register_score = math.exp(-0.5 * (distance_cents / 350.0) ** 2)
    return 0.70 * voiced_ratio + 0.30 * register_score


def select_best_prompt(
    request: dict[str, Any],
    target_audio_path: Path,
    target_audio: np.ndarray,
    target_f0: np.ndarray,
    target_register_f0: np.ndarray | None = None,
    f0_extractor: Callable[[Path, np.ndarray], np.ndarray] | None = None,
) -> tuple[np.ndarray, np.ndarray, float, Path]:
    """Pick a dense profile prompt in the register of the generated part."""
    candidates: list[Path] = []
    raw_candidates = request.get("prompt_audio_candidates", [])
    if isinstance(raw_candidates, list):
        for value in raw_candidates:
            if isinstance(value, str) and value:
                candidates.append(Path(value).expanduser().resolve())

    register_curve = target_register_f0 if target_register_f0 is not None else target_f0
    register_voiced = register_curve[register_curve > 32.0]
    target_register_hz = (
        float(np.median(register_voiced))
        if len(register_voiced)
        else None
    )

    best: tuple[np.ndarray, np.ndarray, float, Path] | None = None
    best_score = -1.0
    for candidate_path in candidates or [target_audio_path]:
        try:
            if candidate_path == target_audio_path:
                candidate_audio = target_audio
                candidate_f0 = target_f0
            else:
                candidate_audio = load_mono_24k(candidate_path)
                if not len(candidate_audio):
                    continue
                candidate_f0 = (
                    f0_extractor(candidate_path, candidate_audio)
                    if f0_extractor is not None
                    else extract_f0(candidate_audio)
                )
            prompt_audio, prompt_f0, prompt_start = select_prompt_window(
                candidate_audio,
                candidate_f0,
                target_register_hz=target_register_hz,
            )
        except (OSError, RuntimeError, ValueError):
            continue

        duration_score = min(1.0, len(prompt_audio) / (8.0 * SOULX_SAMPLE_RATE))
        score = prompt_score(prompt_f0, target_register_hz) * 0.95 + duration_score * 0.05
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
        rmvpe_model_path: Path = DEFAULT_RMVPE_MODEL_PATH,
        device: str = "auto",
        steps: int = DEFAULT_INFERENCE_STEPS,
        cfg: float = DEFAULT_INFERENCE_CFG,
    ) -> None:
        self.source_root = source_root.expanduser().resolve()
        self.model_path = model_path.expanduser().resolve()
        self.config_path = config_path.expanduser().resolve()
        self.rmvpe_model_path = rmvpe_model_path.expanduser().resolve()
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
        from preprocess.tools.f0_extraction import F0Extractor  # pylint: disable=import-outside-toplevel
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
        self.rmvpe = (
            F0Extractor(
                str(self.rmvpe_model_path),
                device="cpu",
                target_sr=SOULX_SAMPLE_RATE,
                hop_size=SOULX_HOP_SIZE,
                max_duration=3_600.0,
                verbose=False,
            )
            if self.rmvpe_model_path.is_file()
            else None
        )
        self.active_adapter_path: Path | None = None

    def extract_source_f0(
        self,
        audio_path: Path,
        audio: np.ndarray,
    ) -> np.ndarray:
        if self.rmvpe is None:
            return extract_f0(audio)
        f0 = np.asarray(
            self.rmvpe.process(str(audio_path), verbose=False),
            dtype=np.float32,
        )
        expected_frames = max(1, int(math.ceil(len(audio) / SOULX_HOP_SIZE)))
        if len(f0) < expected_frames:
            f0 = np.pad(f0, (0, expected_frames - len(f0)))
        return np.ascontiguousarray(f0[:expected_frames])

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

        source_f0 = self.extract_source_f0(audio_path, audio)
        expression_profile, expression_profile_path = (
            load_expression_profile_for_adapter(self.active_adapter_path)
        )
        selection_profile = expression_profile or default_expression_profile()
        target_f0, note_mask = build_target_f0(
            notes,
            source_f0,
            selection_profile,
            f0_rate=SOULX_F0_RATE,
        )
        if np.count_nonzero(target_f0) < 5:
            raise ValueError("backing notes do not overlap voiced source audio")

        prompt_audio, prompt_f0, prompt_start, prompt_source = select_best_prompt(
            request,
            audio_path,
            audio,
            source_f0,
            target_register_f0=target_f0,
            f0_extractor=self.extract_source_f0,
        )
        if expression_profile is None:
            expression_profile = analyze_expression_profile(
                [prompt_f0],
                f0_rate=SOULX_F0_RATE,
            )
            target_f0, note_mask = build_target_f0(
                notes,
                source_f0,
                expression_profile,
                f0_rate=SOULX_F0_RATE,
            )

        prompt_wav = torch.from_numpy(prompt_audio).unsqueeze(0).to(self.device)
        target_wav = torch.from_numpy(audio).unsqueeze(0).to(self.device)
        prompt_f0_tensor = torch.from_numpy(prompt_f0).unsqueeze(0).to(self.device)
        target_f0_tensor = torch.from_numpy(target_f0).unsqueeze(0).to(self.device)
        inference_steps = int(request.get("steps", self.steps))
        inference_cfg = float(
            request.get(
                "cfg",
                DEFAULT_LORA_CFG
                if self.active_adapter_path is not None
                else self.cfg,
            )
        )

        with torch.inference_mode():
            generated, _ = self.model.infer(
                pt_wav=prompt_wav,
                gt_wav=target_wav,
                pt_f0=prompt_f0_tensor,
                gt_f0=target_f0_tensor,
                auto_shift=False,
                pitch_shift=0,
                n_steps=inference_steps,
                cfg=inference_cfg,
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

        prompt_voiced = prompt_f0[prompt_f0 > 32.0]
        target_voiced = target_f0[target_f0 > 32.0]
        prompt_register_hz = (
            float(np.median(prompt_voiced))
            if len(prompt_voiced)
            else 0.0
        )
        target_register_hz = (
            float(np.median(target_voiced))
            if len(target_voiced)
            else 0.0
        )
        register_distance_cents = (
            abs(1_200.0 * math.log2(prompt_register_hz / target_register_hz))
            if prompt_register_hz > 0.0 and target_register_hz > 0.0
            else 0.0
        )

        output_path.parent.mkdir(parents=True, exist_ok=True)
        sf.write(str(output_path), rendered.astype(np.float32), SOULX_SAMPLE_RATE)
        return {
            "output": str(output_path),
            "sample_rate": SOULX_SAMPLE_RATE,
            "duration": len(rendered) / SOULX_SAMPLE_RATE,
            "notes": len(notes),
            "engine": "SoulX-Singer-SVC",
            "device": self.device,
            "steps": inference_steps,
            "cfg": inference_cfg,
            "prompt_start": prompt_start,
            "prompt_duration": len(prompt_audio) / SOULX_SAMPLE_RATE,
            "prompt_source": str(prompt_source),
            "prompt_register_hz": prompt_register_hz,
            "target_register_hz": target_register_hz,
            "register_distance_cents": register_distance_cents,
            "expression_profile": (
                str(expression_profile_path)
                if expression_profile_path is not None
                else "prompt-analysis"
            ),
            "f0_extractor": "rmvpe" if self.rmvpe is not None else "world-dio",
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
    parser.add_argument("--rmvpe-model", type=Path, default=DEFAULT_RMVPE_MODEL_PATH)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--steps", type=int, default=DEFAULT_INFERENCE_STEPS)
    args = parser.parse_args()

    renderer = SoulXRenderer(
        source_root=args.source_root,
        model_path=args.model,
        config_path=args.config,
        rmvpe_model_path=args.rmvpe_model,
        device=args.device,
        steps=args.steps,
    )
    request = json.loads(args.request_json.read_text(encoding="utf-8"))
    print(json.dumps(renderer.render(request), ensure_ascii=False))


if __name__ == "__main__":
    main()
