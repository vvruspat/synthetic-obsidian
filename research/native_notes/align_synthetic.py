"""Transfer human boundaries to converted singing using content/F0 time alignment.

This estimates a time map, not new notes. No boundary-model predictions are used.
Outputs remain synthetic drafts: a low alignment cost is not human validation.
"""
from __future__ import annotations

import argparse
import copy
import json
from pathlib import Path
import sys

import librosa
import numpy as np
import soundfile as sf
import torch

from .corrections import digest

HOP_SECONDS = .02
CONTRACT = "whisper-middle-rmvpe-banded-dtw-v1"


class ContentEncoder:
    def __init__(self, repo):
        # Cached dependencies only; the annotator's audio/message threads are uninvolved.
        from transformers import AutoFeatureExtractor, WhisperModel
        self.device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
        snapshots = sorted((repo / "checkpoints/hf_cache/models--openai--whisper-small/snapshots").glob("*"))
        if not snapshots: raise FileNotFoundError("Cached Whisper-small is required")
        self.model_path = snapshots[-1]
        self.extractor = AutoFeatureExtractor.from_pretrained(self.model_path, local_files_only=True)
        self.model = WhisperModel.from_pretrained(self.model_path).encoder.eval().to(self.device)
        sys.path.insert(0, str(repo / "third_party/seed-vc"))
        from modules.rmvpe import RMVPE
        checkpoints = sorted((repo / "checkpoints/models--lj1995--VoiceConversionWebUI/snapshots").glob("*/rmvpe.pt"))
        if not checkpoints: raise FileNotFoundError("Cached RMVPE is required")
        self.pitch = RMVPE(str(checkpoints[-1]), is_half=False, device="cpu")
        self.pitch_path = checkpoints[-1]

    def encode(self, audio, rate):
        if audio.ndim == 2:
            audio = audio[:, np.argmax(np.square(audio, dtype=np.float64).sum(axis=0))]
        wave = librosa.resample(audio.astype(np.float32), orig_sr=rate, target_sr=16000)
        count = len(wave) // 320 + 1
        times = np.arange(count) * HOP_SECONDS
        content_parts, f0_parts = [], []
        # Fixed global grid, with context on both sides. Do not truncate long human clips.
        for begin in range(0, count, 1000):
            end = min(count, begin + 1000)
            left, right = max(0, begin - 250), min(count, end + 250)
            chunk = wave[left * 320:min(len(wave), right * 320)]
            inputs = self.extractor(chunk, sampling_rate=16000, return_tensors="pt")
            with torch.inference_mode():
                encoded = self.model(inputs.input_features.to(self.device), output_hidden_states=True)
            # Intermediate representations retain local sound structure, without decoding lyrics.
            hidden = torch.cat([encoded.hidden_states[4], encoded.hidden_states[8]], dim=-1)[0].cpu().numpy()
            indices = np.clip(np.arange(begin, end) - left, 0, len(hidden) - 1)
            content_parts.append(hidden[indices])
            pitch_left, pitch_right = max(0, begin - 50), min(count, end + 50)
            pitch_wave = wave[pitch_left * 320:min(len(wave), pitch_right * 320)]
            track = self.pitch.infer_from_audio(pitch_wave, thred=.03)
            f0_parts.append(np.interp(times[begin:end] - pitch_left * HOP_SECONDS,
                                     np.arange(len(track)) * .01, track))
        content, f0 = np.concatenate(content_parts), np.concatenate(f0_parts)
        spectrum = np.abs(librosa.stft(wave, n_fft=512, hop_length=320)) ** 2
        energy = np.log(np.maximum(np.sqrt(spectrum.sum(axis=0)[:count]), 1e-7))
        return {"content": content, "f0": f0, "energy": energy, "times": times, "duration": len(wave) / 16000}


def paired_content(a, b):
    # Remove a constant timbre/level offset; pool scale to avoid independent time normalization.
    a = a - np.mean(a, axis=0)
    b = b - np.mean(b, axis=0)
    scale = np.maximum(np.sqrt((np.var(a, axis=0) + np.var(b, axis=0)) / 2), .1)
    a, b = np.clip(a / scale, -4, 4), np.clip(b / scale, -4, 4)
    a /= np.maximum(np.linalg.norm(a, axis=1, keepdims=True), 1e-8)
    b /= np.maximum(np.linalg.norm(b, axis=1, keepdims=True), 1e-8)
    return a, b


def time_path(cost, max_shift=.16):
    n, m = cost.shape
    delta = np.arange(m)[None] - np.arange(n)[:, None]
    bounded = cost + .03 * (delta * HOP_SECONDS / .1) ** 2
    bounded[np.abs(delta) * HOP_SECONDS > max_shift + 1e-9] = np.inf
    _, backward = librosa.sequence.dtw(C=bounded, step_sizes_sigma=np.array([[1, 1], [1, 2], [2, 1]]),
        weights_add=np.array([0., .03, .03]), weights_mul=np.array([1., 1.5, 1.5]))
    path = backward[::-1]
    # No horizontal/vertical steps: interpolation has positive slope and cannot reorder notes.
    if np.any(np.diff(path, axis=0) <= 0): raise ValueError("Nonmonotone alignment")
    return path


def align(source, rendered, semitones):
    a, b = paired_content(source["content"], rendered["content"])
    content_cost = np.clip(1 - a @ b.T, 0, 2)
    af, bf = source["f0"], rendered["f0"]
    av, bv = af > 50, bf > 50
    am = 69 + 12 * np.log2(np.maximum(af, 1) / 440) + semitones
    bm = 69 + 12 * np.log2(np.maximum(bf, 1) / 440)
    pitch_cost = np.where(av[:, None] & bv[None], np.minimum(abs(am[:, None] - bm[None]) / 3, 1),
                          np.not_equal(av[:, None], bv[None]).astype(float))
    ae, be = source["energy"], rendered["energy"]
    ae = (ae - ae.mean()) / max(float(ae.std()), .1)
    be = (be - be.mean()) / max(float(be.std()), .1)
    energy_cost = np.minimum(abs(ae[:, None] - be[None]) / 2, 1)
    path = time_path(content_cost + .2 * pitch_cost + .1 * energy_cost)
    alternate = time_path(content_cost)
    source_times = path[:, 0] * HOP_SECONDS
    target_times = path[:, 1] * HOP_SECONDS
    alt_times = np.interp(source_times, alternate[:, 0] * HOP_SECONDS, alternate[:, 1] * HOP_SECONDS)
    return {"source_times": source_times, "target_times": target_times,
            "alternate_times": alt_times, "path": path, "content_cost": content_cost,
            "pitch_cost": pitch_cost, "voicing_source": av, "voicing_rendered": bv}


def map_notes(notes, mapping, duration, semitones):
    src, dst, alt = (mapping[key] for key in ("source_times", "target_times", "alternate_times"))
    def warp(time): return float(np.clip(np.interp(time, src, dst), 0, duration))
    output, audit = [], []
    for original in notes:
        note = copy.deepcopy(original)
        note["start"], note["end"] = warp(original["start"]), warp(original["end"])
        if note["end"] <= note["start"]: raise ValueError("Alignment collapsed a note")
        shifts, disagreements, costs = [], [], []
        for head in ("start", "end"):
            time = original[head]
            shifts.append((note[head] - time) * 1000)
            disagreements.append(abs(warp(time) - np.interp(time, src, alt)) * 1000)
            i = min(len(mapping["content_cost"]) - 1, round(time / HOP_SECONDS))
            j = min(mapping["content_cost"].shape[1] - 1, round(note[head] / HOP_SECONDS))
            costs.append(float(mapping["content_cost"][i, j]))
        reasons = []
        if max(abs(v) for v in shifts) > 80: reasons.append("large_time_move")
        if max(disagreements) > 40: reasons.append("cue_disagreement")
        if max(costs) > .8: reasons.append("weak_content_match")
        if note["end"] - note["start"] < .04: reasons.append("short_mapped_note")
        note["flags"] = ["synthetic_aligned", "needs_review"] + reasons
        # Pitch is display metadata only. Drop the original curve rather than present it as measured synthesis.
        note.pop("curve", None)
        for key in ("pitch", "pitch_exact"):
            if key in note: note[key] += semitones
        for key in ("voiced_start", "voiced_end"):
            if key in note: note[key] = float(np.clip(warp(note[key]), note["start"], note["end"]))
        output.append(note)
        audit.append({"id": note["id"], "source": [original["start"], original["end"]],
            "aligned": [note["start"], note["end"]], "moves_ms": shifts,
            "cue_disagreement_ms": disagreements, "content_cost": costs, "review_reasons": reasons})
    return output, audit


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pilot-config", type=Path, required=True)
    parser.add_argument("--rendered", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.output.exists(): raise ValueError("Use a new output directory")
    config = json.loads(args.pilot_config.read_text())
    base_path = Path(config["base_manifest"])
    if digest(base_path) != config["base_manifest_sha256"]: raise ValueError("Frozen base changed")
    records = {r["id"]: r for r in json.loads(base_path.read_text())}
    parent, reference = (records[config[key]["id"]] for key in ("source_record", "reference_record"))
    for record in (parent, reference):
        if record["split"] != "train": raise ValueError("Sources and voice references must be training-only")
        for key in ("audio", "annotation"):
            if digest(Path(record[key])) != record[key + "_sha256"]: raise ValueError("Pinned training input changed")
    source, rate = sf.read(parent["audio"], dtype="float32", always_2d=True)
    rendered, rendered_rate = sf.read(args.rendered, dtype="float32", always_2d=True)
    torch.set_num_threads(4)
    repo = Path(__file__).resolve().parents[2]
    encoder = ContentEncoder(repo)
    src = encoder.encode(source, rate)
    dst = encoder.encode(rendered, rendered_rate)
    mapping = align(src, dst, config["pitch_shift"])
    notes, audit = map_notes(parent["notes"], mapping, len(rendered) / rendered_rate, config["pitch_shift"])
    args.output.mkdir(parents=True)
    np.savez(args.output / "features.npz", source_content=src["content"], source_f0=src["f0"],
             source_energy=src["energy"], rendered_content=dst["content"], rendered_f0=dst["f0"], rendered_energy=dst["energy"])
    wav_path = args.output / "synthetic.wav"
    sf.write(wav_path, rendered, rendered_rate, subtype="PCM_24")
    time_map = {"contract": CONTRACT, "source_times": mapping["source_times"].tolist(),
                "target_times": mapping["target_times"].tolist(), "hop_seconds": HOP_SECONDS,
                "source_sha256": parent["audio_sha256"], "synthetic_sha256": digest(wav_path)}
    (args.output / "time-map.json").write_text(json.dumps(time_map, indent=2))
    annotation = {"version": 1, "audio": wav_path.name, "sample_rate": rendered_rate,
        "duration": len(rendered) / rendered_rate, "notes": notes,
        "dataset": {"review_status": "synthetic_aligned_needs_review", "audio_sha256": digest(wav_path),
                    "source_sha256": digest(wav_path)},
        "augmentation": {"parent_id": parent["id"], "reference_id": reference["id"],
            "parent_annotation_sha256": parent["annotation_sha256"], "semitones": config["pitch_shift"],
            "alignment_contract": CONTRACT, "time_map_sha256": digest(args.output / "time-map.json")}}
    (args.output / "synthetic.annotation.json").write_text(json.dumps(annotation, indent=2))
    # Positive control: a known smooth time warp of the pitched source, independent of the note model.
    pitched, pitch_rate = sf.read(args.pilot_config.parent / "pitch-only.wav", dtype="float32", always_2d=True)
    time = np.arange(len(pitched)) / pitch_rate
    known_map = time + .08 * np.sin(np.pi * time / time[-1]) ** 2
    control_wave = np.stack([np.interp(time, known_map, channel) for channel in pitched.T], axis=1).astype(np.float32)
    control = encoder.encode(control_wave, pitch_rate)
    control_mapping = align(src, control, config["pitch_shift"])
    events = np.array([n[k] for n in parent["notes"] for k in ("start", "end")])
    true = np.interp(events, time, known_map)
    predicted = np.interp(events, control_mapping["source_times"], control_mapping["target_times"])
    errors = (predicted - true) * 1000
    report = {"contract": CONTRACT, "notes": len(notes), "source_notes": len(parent["notes"]),
        "flagged_notes": sum(bool(a["review_reasons"]) for a in audit), "boundaries": audit,
        "known_warp_control": {"maximum_imposed_warp_ms": 80, "before_mae_ms": float(np.mean(abs(events - true)) * 1000),
            "aligned_mae_ms": float(np.mean(abs(errors))), "aligned_p95_abs_error_ms": float(np.percentile(abs(errors), 95)),
            "errors_ms": errors.tolist()},
        "scope": "Automatic synthetic draft, not human-reviewed ground truth. Confidence flags are heuristic.",
        "model_used_for_alignment": str(encoder.model_path), "rmvpe": str(encoder.pitch_path),
        "no_boundary_model_predictions": True, "new_validation_clip_used": False}
    (args.output / "alignment-report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != "boundaries"}, indent=2), flush=True)


if __name__ == "__main__": main()
