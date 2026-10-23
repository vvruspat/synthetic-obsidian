"""Build a resumable local singing dataset from pinned human annotations.

Every derivative comes from train, including voice references. Automatic labels
remain distinct from human labels; uncertain spans are masked out of supervision.
"""
from __future__ import annotations

import argparse
from collections import OrderedDict
import copy
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import sys
import time
from types import SimpleNamespace

import librosa
import numpy as np
import soundfile as sf
import torch

from .align_synthetic import ContentEncoder, CONTRACT, HOP_SECONDS, align, map_notes
from .augmentation import SCHEMA, read_prepared
from .corrections import digest

VERSION = "sasha-human-svc-aligned-dataset-v1"
PROFILES = ("svc-low", "svc-mid", "svc-natural", "svc-high", "pitch-low", "pitch-mid")


def cached_tensor_call(function, capacity=10):
    """Bounded offline inference cache; keys include the complete input waveform/features."""
    cache = OrderedDict()
    def call(tensor, *args, **kwargs):
        array = tensor.detach().float().cpu().contiguous().numpy()
        identity = (array.shape, hashlib.sha256(array.tobytes()).digest(), args, tuple(sorted(kwargs.items())))
        if identity in cache:
            cache.move_to_end(identity)
            return cache[identity]
        value = function(tensor, *args, **kwargs)
        cache[identity] = value
        if len(cache) > capacity: cache.popitem(last=False)
        return value
    return call


def save_json(path, value):
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False, ensure_ascii=False))
    temporary.replace(path)


def mono_read(path):
    audio, rate = sf.read(path, dtype="float32", always_2d=True)
    return audio[:, np.argmax(np.square(audio, dtype=np.float64).sum(axis=0))], rate


def pitch_stats(record, voicing):
    frames = np.asarray(json.loads((voicing / f"{record['id'][:24]}.json").read_text())["yin_frames"])
    times = np.arange(len(frames)) * .01
    inside = np.zeros(len(times), bool)
    for n in record["notes"]: inside |= (times >= n["start"]) & (times < n["end"])
    valid = (frames[:, 2] > .5) & (frames[:, 1] >= .85) & inside
    hz = 440 * 2 ** ((frames[valid, 0] - 69) / 12)
    if len(hz) < 20: raise ValueError("Too little voiced training audio for augmentation")
    # Choose an eight-second crop with high voiced coverage, including short sources.
    width = min(len(times), 800)
    prefix = np.r_[0, np.cumsum(valid)]
    candidates = np.arange(0, max(1, len(times) - width + 1), 5)
    begin = int(candidates[np.argmax(prefix[candidates + width] - prefix[candidates])])
    return {"median_hz": float(np.median(hz)), "q20_hz": float(np.percentile(hz, 20)),
            "reference_crop": [begin * .01, min(record["seconds"], (begin + width) * .01)],
            "voiced_frames": int(valid.sum())}


def semitones_for(stats, profile):
    if profile == "svc-natural": return 0
    target = 130 if profile.endswith("low") else 190 if profile == "svc-mid" else 280 if profile == "svc-high" else 220
    shift = int(np.clip(round(12 * math.log2(target / stats["median_hz"])), -18, 12))
    while shift < 0 and stats["q20_hz"] * 2 ** (shift / 12) < 78: shift += 1
    if profile.startswith("pitch") and shift == 0:
        shift = -3 if profile.endswith("low") and stats["q20_hz"] > 95 else 3
    return shift


def merge_masks(intervals, seconds):
    result = []
    for start, end in sorted(intervals):
        start, end = max(0., start - .08), min(seconds, end + .08)
        if result and start <= result[-1][1]: result[-1][1] = max(result[-1][1], end)
        else: result.append([start, end])
    return result


def audit_labels(parent, source, target, mapping, shift, seconds):
    notes, audit = map_notes(parent["notes"], mapping, seconds, shift)
    source_midi = 69 + 12 * np.log2(np.maximum(source["f0"], 1) / 440) + shift
    target_midi = 69 + 12 * np.log2(np.maximum(target["f0"], 1) / 440)
    for note, item in zip(notes, audit, strict=True):
        inside = (source["times"] >= item["source"][0]) & (source["times"] < item["source"][1]) & (source["f0"] > 50)
        warped = np.interp(source["times"][inside], mapping["source_times"], mapping["target_times"])
        actual_f0 = np.interp(warped, target["times"], target["f0"])
        actual_midi = np.interp(warped, target["times"], target_midi)
        both = actual_f0 > 50
        if len(warped) >= 5:
            item["voicing_retained_fraction"] = float(np.mean(both))
            if np.mean(both) < .5: item["review_reasons"].append("lost_voicing")
            if both.sum() >= 5:
                errors = abs(source_midi[inside][both] - actual_midi[both])
                item["pitch_error_median_semitones"] = float(np.median(errors))
                if np.mean(errors > 2) > .35: item["review_reasons"].append("pitch_contour_mismatch")
        note["flags"] = ["synthetic_aligned", "automatic_label"] + item["review_reasons"]
    masks = merge_masks([item["aligned"] for item in audit if item["review_reasons"]], seconds)
    ignored = sum(end - start for start, end in masks)
    fraction = 1 - ignored / seconds
    reliable = sum(not item["review_reasons"] for item in audit)
    quality = {"usable_time_fraction": fraction, "reliable_notes": reliable, "total_notes": len(notes),
               "accepted": fraction >= .7 and reliable / max(1, len(notes)) >= .7,
               "policy": "heuristic_content_time_voicing_contour_checks_not_human_review"}
    return notes, audit, masks, quality


def frozen_originals(output, base_path, validation_record):
    rows = json.loads(base_path.read_text())
    if validation_record["audio_sha256"] in {r["audio_sha256"] for r in rows}:
        raise ValueError("Reserved validation audio already occurs in the base")
    rows.append(validation_record)
    for row in rows:
        for key in ("audio", "annotation"):
            if digest(Path(row[key])) != row[key + "_sha256"]: raise ValueError("Original input changed")
        directory = output / "originals" / row["split"] / row["id"][:24]
        directory.mkdir(parents=True, exist_ok=True)
        for key in ("audio", "annotation"):
            source = Path(row[key]); destination = directory / source.name
            if not destination.exists(): shutil.copy2(source, destination)
            if digest(destination) != row[key + "_sha256"]: raise ValueError("Frozen copy changed")
            row[key] = str(destination.relative_to(output))
        if row["split"] == "validation": row.setdefault("validation_cohort", "legacy")
    return rows


class SeedRenderer:
    def __init__(self, repo):
        os.environ["HF_HUB_OFFLINE"] = "1"; os.environ["TRANSFORMERS_OFFLINE"] = "1"
        sys.path.insert(0, str(repo / "third_party/seed-vc"))
        import inference
        from modules import rmvpe
        original = rmvpe.RMVPE
        def cpu_rmvpe(*args, **kwargs):
            kwargs["device"] = "cpu"
            return original(*args, **kwargs)
        rmvpe.RMVPE = cpu_rmvpe
        args = SimpleNamespace(f0_condition=True, checkpoint=None, config=None, fp16=False)
        loaded = inference.load_models(args)
        self.campplus = loaded[4]
        self.loaded = (loaded[0], cached_tensor_call(loaded[1]), cached_tensor_call(loaded[2]),
                       loaded[3], cached_tensor_call(loaded[4]), loaded[5], loaded[6])
        # Reuse the same sessions for the entire corpus, rather than reload for every job.
        inference.load_models = lambda unused: self.loaded
        self.inference = inference
        self.device = inference.device
        self.write = sf.write
        def floating_write(file, data, samplerate, *args, **kwargs):
            kwargs.setdefault("subtype", "FLOAT")
            return self.write(file, data, samplerate, *args, **kwargs)
        # The upstream CLI defaults to PCM16, which clips before headroom can be applied.
        inference.sf.write = floating_write

    def embedding(self, audio, rate):
        import torchaudio
        wave = librosa.resample(audio.astype(np.float32), orig_sr=rate, target_sr=16000)
        with torch.inference_mode():
            bank = torchaudio.compliance.kaldi.fbank(torch.from_numpy(wave)[None].to(self.device),
                                                     num_mel_bins=80, dither=0, sample_frequency=16000)
            vector = self.campplus((bank - bank.mean(dim=0, keepdim=True))[None])[0].float().cpu().numpy()
        return vector / max(float(np.linalg.norm(vector)), 1e-8)

    def render(self, source, reference, output, shift, seed):
        torch.manual_seed(seed); np.random.seed(seed)
        args = SimpleNamespace(source=str(source), target=str(reference), output=str(output), diffusion_steps=30,
            length_adjust=1.0, inference_cfg_rate=.7, f0_condition=True, auto_f0_adjust=False,
            semi_tone_shift=shift, checkpoint=None, config=None, fp16=False)
        self.inference.main(args)
        matches = list(output.glob("vc_*.wav"))
        if len(matches) != 1: raise ValueError("Unexpected Seed-VC output")
        return mono_read(matches[0])


def select_references(output, rows, stats, renderer):
    candidates = []
    for row in rows:
        if row["split"] != "train": continue
        audio, rate = mono_read(output / row["audio"])
        begin, end = stats[row["id"]]["reference_crop"]
        crop = audio[round(begin * rate):round(end * rate)]
        vector = renderer.embedding(crop, rate)
        candidates.append({"id": row["id"], "median_hz": stats[row["id"]]["median_hz"],
                           "crop": [begin, end], "embedding": vector.tolist()})
    ordered = sorted(candidates, key=lambda r: r["median_hz"])
    chosen = []
    for pool in (ordered[:len(ordered)//2], ordered[len(ordered)//2:]):
        current = [pool[0] if not chosen else pool[-1]]
        while len(current) < 3:
            remaining = [r for r in pool if r not in current]
            current.append(max(remaining, key=lambda r: min(1 - float(np.dot(r["embedding"], c["embedding"])) for c in current)))
        chosen.extend(current)
    for number, ref in enumerate(sorted(chosen, key=lambda r: r["median_hz"])):
        row = next(r for r in rows if r["id"] == ref["id"])
        audio, rate = mono_read(output / row["audio"])
        begin, end = ref["crop"]
        directory = output / "references"; directory.mkdir(exist_ok=True)
        path = directory / f"r{number+1:02}.wav"
        renderer.write(path, audio[round(begin*rate):round(end*rate)], rate, subtype="PCM_24")
        ref.update(name=path.stem, audio=str(path.relative_to(output)), audio_sha256=digest(path),
                   parent_audio_sha256=row["audio_sha256"])
    return sorted(chosen, key=lambda r: r["median_hz"])


def feature_cache(encoder, audio, rate, path):
    if path.exists():
        with np.load(path) as saved: return {key: saved[key] for key in saved.files}
    value = encoder.encode(audio, rate)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp.npz")
    np.savez_compressed(temporary, **value)
    temporary.replace(path)
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--validation-manifest", type=Path, required=True)
    parser.add_argument("--voicing", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--limit", type=int, help="Process only the first N jobs, for a resumable preflight")
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    os.chdir(repo)
    output = args.output.resolve(); output.mkdir(parents=True, exist_ok=True)
    torch.set_num_threads(4)
    plan_path = output / "plan.json"
    if plan_path.exists():
        plan = json.loads(plan_path.read_text())
        if plan["version"] != VERSION or plan["original_base_sha256"] != digest(args.base): raise ValueError("Different source dataset")
        rows = json.loads((output / "base-originals.json").read_text())
    else:
        previous = json.loads(args.validation_manifest.read_text())["records"]
        validation = [r for r in previous if r.get("validation_cohort") == "low_male_vox243"]
        if len(validation) != 1 or validation[0]["split"] != "validation": raise ValueError("Missing explicit held-out vox_243")
        rows = frozen_originals(output, args.base, validation[0])
        save_json(output / "base-originals.json", rows)
        stats = {r["id"]: pitch_stats(r, args.voicing) for r in rows if r["split"] == "train"}
        save_json(output / "source-statistics.json", stats)
        plan = {"version": VERSION, "original_base_sha256": digest(args.base), "base_manifest_sha256": digest(output / "base-originals.json"),
                "profiles": list(PROFILES), "seed": 1701, "created_at": "2026-10-22", "jobs": []}
    if digest(output / "base-originals.json") != plan["base_manifest_sha256"]: raise ValueError("Frozen originals manifest changed")
    for row in rows:
        for key in ("audio", "annotation"):
            if digest(output / row[key]) != row[key + "_sha256"]: raise ValueError("Original snapshot changed")
    stats = json.loads((output / "source-statistics.json").read_text())
    artifact_patterns = (
        "checkpoints/models--Plachta--Seed-VC/snapshots/*/*f0_44k_bigvgan_pruned_ft_ema_v2.pth",
        "checkpoints/models--Plachta--Seed-VC/snapshots/*/*f0_44k.yml",
        "checkpoints/models--lj1995--VoiceConversionWebUI/snapshots/*/rmvpe.pt",
        "checkpoints/models--funasr--campplus/snapshots/*/campplus_cn_common.bin",
        "checkpoints/hf_cache/models--openai--whisper-small/snapshots/*/model.safetensors",
        "checkpoints/hf_cache/models--nvidia--bigvgan_v2_44khz_128band_512x/snapshots/*/bigvgan_generator.pt")
    artifacts = {}
    for pattern in artifact_patterns:
        matches = sorted(repo.glob(pattern))
        if not matches: raise FileNotFoundError(f"Missing cached model: {pattern}")
        artifacts[str(matches[-1].relative_to(repo))] = digest(matches[-1])
    if plan.get("model_artifacts", artifacts) != artifacts: raise ValueError("Generation/alignment model weights changed")
    plan["model_artifacts"] = artifacts
    save_json(plan_path, plan)
    renderer = SeedRenderer(repo)
    if (output / "references.json").exists(): references = json.loads((output / "references.json").read_text())
    else:
        references = select_references(output, rows, stats, renderer)
        save_json(output / "references.json", references)
    by_id = {r["id"]: r for r in rows}
    if not plan["jobs"]:
        sources = sorted([r for r in rows if r["split"] == "train"], key=lambda r: (r["seconds"], r["id"]))
        for i, row in enumerate(sources):
            for profile in PROFILES:
                reference = None
                if profile.startswith("svc"):
                    pool = references[:3] if profile == "svc-low" else references[3:] if profile == "svc-high" else references
                    pool = [r for r in pool if r["id"] != row["id"]]
                    reference = pool[(i + PROFILES.index(profile)) % len(pool)]
                token = hashlib.sha256((row["id"] + profile + (reference["id"] if reference else "")).encode()).hexdigest()
                plan["jobs"].append({"id": token[:24], "parent_id": row["id"], "profile": profile,
                    "semitones": semitones_for(stats[row["id"]], profile), "reference": reference["name"] if reference else None,
                    "seed": 1701 + int(token[:7], 16)})
        save_json(plan_path, plan)
    for ref in references:
        if by_id[ref["id"]]["split"] != "train" or digest(output / ref["audio"]) != ref["audio_sha256"]:
            raise ValueError("Reference changed or is not training-only")
    if plan.get("references_sha256", digest(output / "references.json")) != digest(output / "references.json"):
        raise ValueError("Reference bank changed")
    plan["references_sha256"] = digest(output / "references.json")
    save_json(plan_path, plan)
    encoder = ContentEncoder(repo)
    implementation = {name: digest(repo / "research/native_notes" / name)
                      for name in ("build_augmented_dataset.py", "align_synthetic.py", "augmentation.py")}
    provenance = output / "provenance"; provenance.mkdir(exist_ok=True)
    for name, checksum in implementation.items():
        destination = provenance / f"{checksum[:12]}-{name}"
        if not destination.exists(): shutil.copy2(repo / "research/native_notes" / name, destination)
    accepted, results = [], []
    started = time.monotonic()
    for index, job in enumerate(plan["jobs"]):
        if args.limit and index >= args.limit: break
        parent = by_id[job["parent_id"]]
        folder = output / "synthetic" / job["profile"] / job["id"]
        result_path = folder / "result.json"
        if result_path.exists():
            result = json.loads(result_path.read_text())
            if result["status"] == "accepted":
                record = result["record"]
                if any(digest(output / record[key]) != record[key + "_sha256"] for key in ("audio", "annotation")):
                    raise ValueError("Completed output changed")
                accepted.append(record)
            if result["status"] in ("accepted", "quarantined"):
                results.append(result); continue
        folder.mkdir(parents=True, exist_ok=True)
        job_started = time.monotonic()
        print(json.dumps({"event": "job_start", "job": index+1, "total": len(plan["jobs"]), **job}), flush=True)
        try:
            wave, rate = mono_read(output / parent["audio"])
            source = feature_cache(encoder, wave, rate, output / "cache" / f"source-{parent['id'][:24]}.npz")
            work = output / "work" / job["id"]; work.mkdir(parents=True, exist_ok=True)
            reference = next((r for r in references if r["name"] == job["reference"]), None)
            if reference:
                source_path = work / "source.wav"
                renderer.write(source_path, wave, rate, subtype="FLOAT")
                converted, converted_rate = renderer.render(source_path, output / reference["audio"], work, job["semitones"], job["seed"])
                converted = librosa.resample(converted, orig_sr=converted_rate, target_sr=rate)
            else:
                converted = librosa.effects.pitch_shift(wave, sr=rate, n_steps=job["semitones"],
                    n_fft=2048, hop_length=256, res_type="soxr_hq")
            unpadded_samples = len(converted)
            converted = np.pad(converted[:len(wave)], (0, max(0, len(wave)-len(converted))))
            if not np.isfinite(converted).all() or np.max(abs(converted)) < 1e-6: raise ValueError("Invalid or silent render")
            gain = min(1., .98 / float(np.max(abs(converted))))
            converted = converted * gain
            audio_path = folder / "clip.wav"
            renderer.write(audio_path, converted, rate, subtype="PCM_24")
            # Align the actual quantized training audio, not an earlier floating waveform.
            rendered, _ = mono_read(audio_path)
            target = encoder.encode(rendered, rate)
            mapping = align(source, target, job["semitones"])
            if mapping["source_times"][-1] < parent["seconds"]:
                mapping["source_times"] = np.r_[mapping["source_times"], parent["seconds"]]
                mapping["target_times"] = np.r_[mapping["target_times"], parent["seconds"]]
                mapping["alternate_times"] = np.r_[mapping["alternate_times"], parent["seconds"]]
            notes, audit, masks, quality = audit_labels(parent, source, target, mapping, job["semitones"], parent["seconds"])
            audio_hash = digest(audio_path)
            alignment_path = folder / "alignment.json"
            save_json(alignment_path, {"contract": CONTRACT, "source_sha256": parent["audio_sha256"],
                "synthetic_sha256": audio_hash, "source_times": mapping["source_times"].tolist(),
                "target_times": mapping["target_times"].tolist(), "boundaries": audit})
            info = {"method": "seed_vc_f0_singing_30steps" if reference else "librosa_phase_vocoder_soxr_hq",
                "parent_id": parent["id"], "parent_audio_sha256": parent["audio_sha256"],
                "parent_annotation_sha256": parent["annotation_sha256"], "semitones": job["semitones"],
                "profile": job["profile"], "seed": job["seed"], "quality": quality,
                "alignment": str(alignment_path.relative_to(output)), "alignment_sha256": digest(alignment_path),
                "implementation": implementation, "render_gain": gain,
                "prepad_length_difference_ms": (unpadded_samples - len(wave)) / rate * 1000}
            if reference: info.update(reference_id=reference["id"], reference_audio_sha256=reference["parent_audio_sha256"],
                                       reference_crop_sha256=reference["audio_sha256"], reference_crop=reference["crop"])
            annotation_path = folder / "clip.annotation.json"
            save_json(annotation_path, {"version": 1, "audio": "clip.wav", "sample_rate": rate,
                "duration": parent["seconds"], "notes": notes,
                "dataset": {"review_status": "synthetic_aligned_automatic", "audio_sha256": audio_hash, "source_sha256": audio_hash},
                "augmentation": info, "training": {"ignore_intervals": masks}})
            record = {"id": audio_hash, "audio": str(audio_path.relative_to(output)), "audio_sha256": audio_hash,
                "annotation": str(annotation_path.relative_to(output)), "annotation_sha256": digest(annotation_path),
                "seconds": parent["seconds"], "sample_rate": rate, "split": "train", "group": parent["group"],
                "review_authority": "automatic_alignment_of_human_boundaries_not_human_reviewed",
                "notes": notes, "augmentation": info, "ignore_intervals": masks}
            result = {"status": "accepted" if quality["accepted"] else "quarantined", "job": job,
                      "record": record, "quality": quality, "elapsed_seconds": time.monotonic()-job_started}
            if quality["accepted"]: accepted.append(record)
        except Exception as error:
            result = {"status": "error", "job": job, "error": str(error), "elapsed_seconds": time.monotonic()-job_started}
        save_json(result_path, result); results.append(result)
        summary = {"version": VERSION, "planned_jobs": len(plan["jobs"]), "processed_jobs": len(results),
            "accepted_jobs": len(accepted), "quarantined_jobs": sum(r["status"]=="quarantined" for r in results),
            "errors": sum(r["status"]=="error" for r in results), "original_train_seconds": sum(r["seconds"] for r in rows if r["split"]=="train"),
            "synthetic_seconds": sum(r["seconds"] for r in accepted),
            "usable_synthetic_seconds": sum(r["seconds"]-sum(b-a for a,b in r["ignore_intervals"]) for r in accepted),
            "elapsed_this_run_seconds": time.monotonic()-started}
        save_json(output / "progress.json", summary)
        print(json.dumps({"event": "job_end", "job": index+1, "status": result["status"],
                          "seconds": result["elapsed_seconds"], "accepted": len(accepted),
                          "synthetic_minutes": round(summary["synthetic_seconds"]/60,2),
                          "error": result.get("error")}), flush=True)
        if (index+1) % 6 == 0:
            save_json(output / "prepared.json", {"schema": SCHEMA, "base_manifest": "base-originals.json",
                "base_manifest_sha256": plan["base_manifest_sha256"], "records": rows + accepted,
                "synthetic_labels": "automatic_alignment_with_uncertain_regions_masked", "recommended_synthetic_sampling_weight": .25})
    save_json(output / "prepared.json", {"schema": SCHEMA, "base_manifest": "base-originals.json",
        "base_manifest_sha256": plan["base_manifest_sha256"], "records": rows + accepted,
        "synthetic_labels": "automatic_alignment_with_uncertain_regions_masked", "recommended_synthetic_sampling_weight": .25})
    validated = read_prepared(output / "prepared.json")
    save_json(output / "build-report.json", {"version": VERSION, "complete": len(results)==len(plan["jobs"]),
        "jobs": [{k:v for k,v in r.items() if k!="record"} for r in results], "records": len(validated),
        "total_seconds": sum(r["seconds"] for r in validated), "validation_and_test_augmented": False,
        "synthetic_is_not_human_reviewed": True, "mask_policy": "ignore all three target heads in unreliable spans"})
    print(json.dumps({"event": "validated", "records": len(validated), "processed": len(results), "total": len(plan["jobs"])}), flush=True)


if __name__ == "__main__": main()
