from __future__ import annotations

import argparse
import json
from pathlib import Path

import numpy as np
import soundfile as sf
import torch

from .pipeline import CONTEXT_FRAMES, DILATIONS, FEATURES, HOP, MELS, N_FFT, SAMPLE_RATE, SCHEMA, SibilantNet, digest, features, merge_sibilant_notes, sibilant_note_coverage, has_voiced_core
from .train import probabilities


def load_model(path):
    contract = json.loads(Path(path).read_text())
    if contract["schema"] != SCHEMA or contract["feature_count"] != FEATURES:
        raise ValueError("Unsupported sibilant model contract")
    for key, expected in (("sample_rate", SAMPLE_RATE), ("hop", HOP), ("n_fft", N_FFT), ("mels", MELS), ("dilations", list(DILATIONS))):
        if contract[key] != expected:
            raise ValueError("Unsupported feature/model configuration: " + key)
    model = SibilantNet(channels=contract["channels"])
    state = {k: torch.tensor(v, dtype=torch.float32) for k, v in contract["state"].items()}
    if not all(bool(torch.isfinite(value).all()) for value in state.values()):
        raise ValueError("Non-finite sibilant model weights")
    model.load_state_dict(state)
    mean = np.asarray(contract["input_mean"], np.float32)
    std = np.asarray(contract["input_std"], np.float32)
    if mean.shape != (FEATURES,) or std.shape != mean.shape or not np.isfinite(mean).all() or not np.isfinite(std).all() or np.any(std <= 0):
        raise ValueError("Invalid feature normalization")
    return model.eval(), mean, std, contract


def predict_audio(path, model, mean, std, device):
    model.to(device)
    with sf.SoundFile(path) as audio:
        count = int(np.ceil(audio.frames * 100 / audio.samplerate))
        output = np.empty(count, np.float32)
        for start in range(0, count, 6000):
            end = min(count, start + 6000)
            lo, hi = max(0, start - CONTEXT_FRAMES - 4), min(count, end + CONTEXT_FRAMES + 4)
            audio.seek(round(lo * audio.samplerate / 100))
            samples = min(audio.frames - audio.tell(), round((hi - lo) * audio.samplerate / 100))
            wave = audio.read(samples, dtype="float32", always_2d=True)
            prediction = probabilities(model, features(wave, audio.samplerate), mean, std, device)
            output[start:end] = prediction[start - lo:end - lo]
    return output


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--audio", type=Path, required=True)
    parser.add_argument("--notes", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    torch.set_num_threads(4)
    model, mean, std, contract = load_model(args.model)
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    output = predict_audio(args.audio, model, mean, std, device)
    args.output.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(args.output / "probabilities.npz", probabilities=output)
    threshold = contract["balanced_threshold"]
    report = {"audio": str(args.audio.resolve()), "audio_sha256": digest(args.audio),
              "model": str(args.model.resolve()), "model_sha256": digest(args.model),
              "conservative_threshold": contract["conservative_threshold"], "detection_threshold": threshold,
              "attachment_coverage": ">0.50", "sibilant_seconds": float(np.sum(output >= threshold) / 100),
              "voiced_core_guard": True,
              "duration_seconds": len(output) / 100}
    if args.notes:
        document = json.loads(args.notes.read_text())
        merged, proposals = merge_sibilant_notes(document["notes"], output, threshold)
        report.update({"input_notes": len(document["notes"]), "proposed_notes": len(merged), "proposals": proposals,
                       "per_note": [{"id": n["id"], "start": n["start"], "end": n["end"],
                                     "coverage": sibilant_note_coverage(n, output, threshold),
                                     "voiced_core": has_voiced_core(n, output, threshold),
                                     "mean_probability": float(np.mean(output[int(np.ceil(n["start"]*100)):int(np.ceil(n["end"]*100))]))}
                                    for n in document["notes"]]})
        (args.output / "proposed-note-blocks.json").write_text(json.dumps({"notes": merged}, indent=2))
    (args.output / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != "per_note"}, indent=2))


if __name__ == "__main__":
    main()
