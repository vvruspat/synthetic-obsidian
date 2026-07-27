#!/usr/bin/env python3
"""Persistent, app-independent LoRA training job for SoulX-Singer-SVC."""

from __future__ import annotations

import argparse
import gc
import hashlib
import json
import math
import os
import shutil
import signal
import sys
import time
import traceback
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

os.environ.setdefault("PYTORCH_ENABLE_MPS_FALLBACK", "1")

import numpy as np
import pyworld as pw
import soundfile as sf
import torch
from scipy.signal import resample_poly

from soulx_lora import (
    LoRAConfig,
    inject_lora,
    save_adapter,
    trainable_lora_parameters,
)

SAMPLE_RATE = 24_000
HOP_SIZE = 480
F0_RATE = SAMPLE_RATE // HOP_SIZE


class TrainingCancelled(RuntimeError):
    pass


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat()


def atomic_json_write(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(
        json.dumps(payload, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    temporary.replace(path)


class JobReporter:
    def __init__(self, job_dir: Path, request: dict[str, Any]) -> None:
        self.job_dir = job_dir
        self.state_path = job_dir / "state.json"
        self.cancel_path = job_dir / "cancel.request"
        self.log_path = job_dir / "training.log"
        self.state: dict[str, Any] = {
            "schemaVersion": 1,
            "jobId": request["jobId"],
            "presetName": request["presetName"],
            "quality": request["quality"],
            "status": "queued",
            "stage": "Queued",
            "message": "Waiting for the training process...",
            "progress": 0.0,
            "currentStep": 0,
            "totalSteps": int(request["training"]["steps"]),
            "createdAt": request.get("createdAt", utc_now()),
            "updatedAt": utc_now(),
            "pid": os.getpid(),
            "profileDirectory": request["profileDirectory"],
            "adapterFile": "",
            "canCancel": True,
        }
        atomic_json_write(self.state_path, self.state)

    def update(
        self,
        *,
        status: str | None = None,
        stage: str | None = None,
        message: str | None = None,
        progress: float | None = None,
        current_step: int | None = None,
        **extra: Any,
    ) -> None:
        if status is not None:
            self.state["status"] = status
        if stage is not None:
            self.state["stage"] = stage
        if message is not None:
            self.state["message"] = message
        if progress is not None:
            self.state["progress"] = float(max(0.0, min(1.0, progress)))
        if current_step is not None:
            self.state["currentStep"] = int(current_step)
        self.state.update(extra)
        self.state["updatedAt"] = utc_now()
        atomic_json_write(self.state_path, self.state)

    def log(self, message: str) -> None:
        with self.log_path.open("a", encoding="utf-8") as handle:
            handle.write(f"[{utc_now()}] {message}\n")
            handle.flush()

    def check_cancelled(self) -> None:
        if self.cancel_path.exists():
            self.update(
                status="cancelling",
                stage="Cancelling",
                message="Stopping safely after the current operation...",
            )
            raise TrainingCancelled("Training cancelled by the user")


def load_mono_24k(path: Path) -> np.ndarray:
    audio, sample_rate = sf.read(str(path), dtype="float32", always_2d=True)
    mono = np.mean(audio, axis=1, dtype=np.float32)
    if sample_rate != SAMPLE_RATE:
        divisor = math.gcd(int(sample_rate), SAMPLE_RATE)
        mono = resample_poly(
            mono,
            SAMPLE_RATE // divisor,
            int(sample_rate) // divisor,
        ).astype(np.float32)
    peak = float(np.max(np.abs(mono))) if len(mono) else 0.0
    if peak > 1.0:
        mono /= peak
    return np.ascontiguousarray(mono)


def extract_f0(audio: np.ndarray) -> np.ndarray:
    frame_period_ms = 1000.0 / F0_RATE
    source = audio.astype(np.float64, copy=False)
    f0, time_axis = pw.dio(
        source,
        SAMPLE_RATE,
        frame_period=frame_period_ms,
        f0_floor=32.0,
        f0_ceil=1_200.0,
    )
    f0 = pw.stonemask(source, f0, time_axis, SAMPLE_RATE)
    expected = max(1, int(math.ceil(len(audio) / HOP_SIZE)))
    if len(f0) < expected:
        f0 = np.pad(f0, (0, expected - len(f0)))
    return f0[:expected].astype(np.float32)


def collect_segments(
    source_files: list[Path],
    *,
    segment_seconds: float,
    maximum_segments: int,
    reporter: JobReporter,
) -> list[tuple[np.ndarray, np.ndarray, str]]:
    segment_samples = int(round(segment_seconds * SAMPLE_RATE))
    hop_samples = max(1, int(round(segment_samples * 0.75)))
    candidates: list[tuple[float, np.ndarray, np.ndarray, str]] = []

    for source_index, path in enumerate(source_files):
        reporter.check_cancelled()
        reporter.update(
            stage="Analysing recordings",
            message=f"Finding clean vocal sections in {path.name}...",
            progress=0.10 + 0.08 * source_index / max(1, len(source_files)),
        )
        audio = load_mono_24k(path)
        if len(audio) < segment_samples:
            audio = np.pad(audio, (0, segment_samples - len(audio)))
        full_f0 = extract_f0(audio)

        for start in range(0, max(1, len(audio) - segment_samples + 1), hop_samples):
            end = min(len(audio), start + segment_samples)
            segment = audio[start:end]
            if len(segment) < segment_samples:
                segment = np.pad(segment, (0, segment_samples - len(segment)))
            f0_start = start // HOP_SIZE
            f0_length = max(1, int(math.ceil(len(segment) / HOP_SIZE)))
            segment_f0 = full_f0[f0_start : f0_start + f0_length]
            if len(segment_f0) < f0_length:
                segment_f0 = np.pad(segment_f0, (0, f0_length - len(segment_f0)))

            voiced_ratio = float(np.mean(segment_f0 > 32.0))
            rms = float(np.sqrt(np.mean(np.square(segment), dtype=np.float64)))
            if voiced_ratio < 0.22 or rms < 0.008:
                continue
            clipped_ratio = float(np.mean(np.abs(segment) >= 0.995))
            score = voiced_ratio + min(rms, 0.2) * 0.5 - clipped_ratio * 4.0
            label = f"{path.name}:{start / SAMPLE_RATE:.2f}s"
            candidates.append((score, segment.copy(), segment_f0.copy(), label))

    candidates.sort(key=lambda item: item[0], reverse=True)
    selected = candidates[:maximum_segments]
    if len(selected) < 2:
        raise RuntimeError(
            "Not enough clean voiced sections were found. Add dry solo-vocal recordings."
        )
    return [(audio, f0, label) for _, audio, f0, label in selected]


def build_soulx_model(request: dict[str, Any], reporter: JobReporter):
    source_root = Path(request["sourceRoot"]).expanduser().resolve()
    sys.path.insert(0, str(source_root))
    from soulxsinger.models.soulxsinger_svc import SoulXSingerSVC
    from soulxsinger.utils.file_utils import load_config

    config = load_config(request["configPath"])
    reporter.check_cancelled()
    reporter.update(
        status="loading",
        stage="Loading SoulX",
        message="Loading the frozen SoulX-Singer-SVC base model...",
        progress=0.19,
    )
    model = SoulXSingerSVC(config)
    checkpoint = torch.load(
        request["modelPath"],
        weights_only=False,
        map_location="cpu",
    )
    model.load_state_dict(checkpoint["state_dict"], strict=True)
    del checkpoint
    gc.collect()
    model.eval()
    for parameter in model.parameters():
        parameter.requires_grad_(False)
    return model, config


def build_feature_cache(
    model,
    segments: list[tuple[np.ndarray, np.ndarray, str]],
    *,
    device: torch.device,
    reporter: JobReporter,
) -> list[dict[str, Any]]:
    model.mel.to(device)
    model.f0_encoder.to(device)
    features: list[dict[str, Any]] = []

    for index, (audio, f0, label) in enumerate(segments):
        reporter.check_cancelled()
        reporter.update(
            status="preprocessing",
            stage="Extracting features",
            message=f"Encoding vocal segment {index + 1} of {len(segments)}...",
            progress=0.22 + 0.28 * index / max(1, len(segments)),
        )
        waveform = torch.from_numpy(audio).unsqueeze(0).to(device)
        with torch.inference_mode():
            mel = model.mel(waveform.float())
            content = model.whisper_encoder.encode(waveform.float(), sr=SAMPLE_RATE)
            coarse_f0 = model.f0_to_coarse(
                torch.from_numpy(f0).unsqueeze(0)
            ).to(device)
            frame_count = min(mel.shape[1], content.shape[1], coarse_f0.shape[1])
            if frame_count < 20:
                continue
            condition = (
                content[:, :frame_count, :]
                + model.f0_encoder(coarse_f0[:, :frame_count])
            )
        features.append(
            {
                "mel": mel[:, :frame_count, :].half().cpu(),
                "condition": condition[:, :frame_count, :].half().cpu(),
                "label": label,
            }
        )
        del waveform, mel, content, condition, coarse_f0

    model.whisper_encoder.model.to("cpu")
    model.mel.to("cpu")
    gc.collect()
    if device.type == "mps":
        torch.mps.empty_cache()
    if len(features) < 2:
        raise RuntimeError("Feature extraction produced fewer than two usable segments")
    return features


def flow_matching_loss(decoder, prompt: dict[str, Any], target: dict[str, Any], device):
    prompt_mel = prompt["mel"].float().to(device)
    target_mel = target["mel"].float().to(device)
    prompt_condition = prompt["condition"].float().to(device)
    target_condition = target["condition"].float().to(device)

    mel = torch.cat([prompt_mel, target_mel, prompt_mel], dim=1)
    condition = torch.cat(
        [prompt_condition, target_condition, prompt_condition],
        dim=1,
    )
    frame_mask = torch.ones(mel.shape[:2], device=device)
    prompt_mask = torch.cat(
        [
            torch.ones(prompt_mel.shape[:2], device=device),
            torch.zeros(target_mel.shape[:2], device=device),
            torch.ones(prompt_mel.shape[:2], device=device),
        ],
        dim=1,
    )

    outputs = decoder(mel, frame_mask, condition, prompt_mask)
    noise, clean_mel, prediction, final_mask, _ = outputs["output"]
    flow_target = clean_mel - (1.0 - decoder.sigma) * noise
    loss_values = torch.nn.functional.l1_loss(
        prediction,
        flow_target,
        reduction="none",
    ).float()
    weighted = loss_values * final_mask
    denominator = final_mask.sum().clamp_min(1.0) * loss_values.shape[-1]
    return weighted.sum() / denominator


def train_adapter(
    model,
    feature_cache: list[dict[str, Any]],
    request: dict[str, Any],
    *,
    device: torch.device,
    reporter: JobReporter,
) -> tuple[LoRAConfig, float]:
    training = request["training"]
    lora_config = LoRAConfig.from_dict(training["lora"])
    model.cfm_decoder.to(device)
    modules = inject_lora(model, lora_config)
    parameters = list(trainable_lora_parameters(model))
    optimizer = torch.optim.AdamW(
        parameters,
        lr=float(training["learningRate"]),
        weight_decay=0.01,
    )
    decoder = model.cfm_decoder.model
    decoder.train()
    decoder.cfg_drop_prob = 0.0
    steps = int(training["steps"])
    accumulation = max(1, int(training.get("gradientAccumulation", 1)))
    random = np.random.default_rng(int(training.get("seed", 1337)))
    running_loss = 0.0
    optimizer.zero_grad(set_to_none=True)
    reporter.log(
        f"Injected {len(modules)} LoRA modules with "
        f"{sum(parameter.numel() for parameter in parameters):,} trainable parameters."
    )

    for step in range(1, steps + 1):
        reporter.check_cancelled()
        target_index = int(random.integers(0, len(feature_cache)))
        prompt_index = int(random.integers(0, len(feature_cache) - 1))
        if prompt_index >= target_index:
            prompt_index += 1
        loss = flow_matching_loss(
            decoder,
            feature_cache[prompt_index],
            feature_cache[target_index],
            device,
        )
        if not torch.isfinite(loss):
            raise RuntimeError("Training produced a non-finite loss")
        (loss / accumulation).backward()

        if step % accumulation == 0 or step == steps:
            torch.nn.utils.clip_grad_norm_(parameters, max_norm=1.0)
            optimizer.step()
            optimizer.zero_grad(set_to_none=True)

        loss_value = float(loss.detach().cpu())
        running_loss = loss_value if step == 1 else running_loss * 0.96 + loss_value * 0.04
        if step == 1 or step % 5 == 0 or step == steps:
            reporter.update(
                status="training",
                stage="Training LoRA",
                message=f"Optimising singer adapter · loss {running_loss:.4f}",
                progress=0.52 + 0.43 * step / max(1, steps),
                current_step=step,
                loss=running_loss,
            )
        del loss

    decoder.eval()
    return lora_config, running_loss


def copy_sources(
    request: dict[str, Any],
    profile_dir: Path,
    reporter: JobReporter,
) -> list[Path]:
    sources_dir = profile_dir / "sources"
    sources_dir.mkdir(parents=True, exist_ok=False)
    copied: list[Path] = []
    for index, source in enumerate(request["sources"]):
        reporter.check_cancelled()
        original = Path(source["path"]).expanduser().resolve()
        target = sources_dir / f"{index + 1:02d}_{original.name}"
        reporter.update(
            status="preparing",
            stage="Preparing dataset",
            message=f"Copying {original.name}...",
            progress=0.02 + 0.06 * index / max(1, len(request["sources"])),
        )
        shutil.copy2(original, target)
        copied.append(target)
    return copied


def write_profile_manifest(
    request: dict[str, Any],
    profile_dir: Path,
    copied_sources: list[Path],
    adapter_path: Path,
    final_loss: float,
) -> None:
    source_entries = []
    request_sources = request["sources"]
    for source, copied in zip(request_sources, copied_sources, strict=True):
        source_entries.append(
            {
                "file": str(copied.relative_to(profile_dir)),
                "originalName": source.get("originalName", copied.name),
                "durationSeconds": source.get("durationSeconds", 0.0),
                "sampleRate": source.get("sampleRate", 0.0),
                "channels": source.get("channels", 0),
            }
        )
    manifest = {
        "schemaVersion": 2,
        "presetName": request["presetName"],
        "kind": "soulx_lora_profile",
        "engine": "soulx_svc_lora",
        "trainingStatus": "complete",
        "fineTuned": True,
        "quality": request["quality"],
        "totalDurationSeconds": request["totalDurationSeconds"],
        "createdAt": request.get("createdAt", utc_now()),
        "completedAt": utc_now(),
        "sources": source_entries,
        "primaryReference": source_entries[0]["file"],
        "adapterFile": str(adapter_path.relative_to(profile_dir)),
        "finalLoss": final_loss,
        "baseModel": request["modelPath"],
    }
    atomic_json_write(profile_dir / "manifest.json", manifest)
    atomic_json_write(
        profile_dir.parent / "active.json",
        {
            "directory": profile_dir.name,
            "presetName": request["presetName"],
            "updatedAt": utc_now(),
        },
    )


def run(job_dir: Path) -> None:
    request_path = job_dir / "request.json"
    request = json.loads(request_path.read_text(encoding="utf-8"))
    reporter = JobReporter(job_dir, request)
    profile_dir = Path(request["profileDirectory"]).expanduser().resolve()
    adapter_path = profile_dir / "adapter" / "soulx-lora.safetensors"

    try:
        reporter.log(f"Starting detached training process {os.getpid()}.")
        reporter.check_cancelled()
        profile_dir.parent.mkdir(parents=True, exist_ok=True)
        copied_sources = copy_sources(request, profile_dir, reporter)
        segments = collect_segments(
            copied_sources,
            segment_seconds=float(request["training"]["segmentSeconds"]),
            maximum_segments=int(request["training"]["maximumSegments"]),
            reporter=reporter,
        )
        reporter.log(f"Selected {len(segments)} voiced training segments.")

        device = torch.device(
            "mps" if torch.backends.mps.is_available() else "cpu"
        )
        model, _ = build_soulx_model(request, reporter)
        feature_cache = build_feature_cache(
            model,
            segments,
            device=device,
            reporter=reporter,
        )
        reporter.check_cancelled()
        lora_config, final_loss = train_adapter(
            model,
            feature_cache,
            request,
            device=device,
            reporter=reporter,
        )
        reporter.check_cancelled()
        reporter.update(
            status="saving",
            stage="Saving adapter",
            message="Writing the trained LoRA adapter...",
            progress=0.96,
        )
        with Path(request["modelPath"]).open("rb") as model_handle:
            base_hash = hashlib.sha256(
                model_handle.read(4 * 1024 * 1024)
            ).hexdigest()
        save_adapter(
            adapter_path,
            model,
            lora_config,
            {
                "presetName": request["presetName"],
                "baseModel": request["modelPath"],
                "baseModelPrefixSha256": base_hash,
                "quality": request["quality"],
                "steps": request["training"]["steps"],
                "finalLoss": final_loss,
                "createdAt": request.get("createdAt", utc_now()),
                "completedAt": utc_now(),
            },
        )
        write_profile_manifest(
            request,
            profile_dir,
            copied_sources,
            adapter_path,
            final_loss,
        )
        reporter.update(
            status="complete",
            stage="Complete",
            message="LoRA training finished. The adapter is active for new renders.",
            progress=1.0,
            current_step=int(request["training"]["steps"]),
            adapterFile=str(adapter_path),
            canCancel=False,
            completedAt=utc_now(),
            loss=final_loss,
        )
        reporter.log(f"Training complete: {adapter_path}")
    except TrainingCancelled:
        reporter.update(
            status="cancelled",
            stage="Cancelled",
            message="Training was cancelled by the user.",
            canCancel=False,
            completedAt=utc_now(),
        )
        reporter.log("Training cancelled by the user.")
    except Exception as exc:  # noqa: BLE001 - persist all failures for the UI.
        reporter.log(traceback.format_exc())
        reporter.update(
            status="error",
            stage="Failed",
            message=str(exc),
            canCancel=False,
            completedAt=utc_now(),
            errorType=type(exc).__name__,
        )
        raise


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--job-dir", type=Path, required=True)
    args = parser.parse_args()
    job_dir = args.job_dir.expanduser().resolve()
    job_dir.mkdir(parents=True, exist_ok=True)
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    if hasattr(signal, "SIGHUP"):
        signal.signal(signal.SIGHUP, signal.SIG_IGN)
    run(job_dir)


if __name__ == "__main__":
    main()
