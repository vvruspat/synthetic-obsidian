"""Lower training clips without time stretching; reserve a new manual clip for validation."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path

import librosa
import numpy as np
import soundfile as sf

from .augmentation import SCHEMA, derivative_record, read_prepared
from .corrections import digest


def alignment_check(source, converted, rate, note_times=()):
    """Envelope timing check, not proof that every phoneme/legato survived a renderer."""
    def envelope(audio):
        if audio.ndim == 2:
            audio = audio[:, np.argmax(np.square(audio, dtype=np.float64).sum(axis=0))]
        audio = librosa.resample(audio, orig_sr=rate, target_sr=16000)
        spectrum = np.abs(librosa.stft(audio, n_fft=512, hop_length=80)) ** 2
        rms = np.sqrt(spectrum.sum(axis=0))
        return rms / max(float(rms.max()), 1e-8)
    a, b = envelope(source), envelope(converted)
    count = min(len(a), len(b))
    lags, correlations = [], []
    centers = sorted(set(range(100, count - 100, 50)) | {
        round(t * 200) for t in note_times if 0.5 <= t <= (count - 100) / 200})
    for center in centers:
        reference = a[center-80:center+80]
        if reference.max() < 0.025 or reference.std() < 0.005:
            continue
        reference = (reference - reference.mean()) / reference.std()
        candidates = []
        for lag in range(-20, 21):
            target = b[center-80+lag:center+80+lag]
            candidates.append(float(np.mean(reference * (target - target.mean()) / max(float(target.std()), 1e-8))))
        best = int(np.argmax(candidates))
        correlations.append(candidates[best])
        if candidates[best] >= 0.8:
            lags.append((best - 20) * 5)
    return {"envelope_correlation": float(np.corrcoef(a[:count], b[:count])[0, 1]),
            "reliable_windows": len(lags), "measured_windows": len(correlations),
            "median_abs_lag_ms": float(np.median(np.abs(lags))) if lags else None,
            "p95_abs_lag_ms": float(np.percentile(np.abs(lags), 95)) if lags else None,
            "reliable_fraction": len(lags) / max(1, len(correlations)),
            "scope": "signal-envelope-only_not_phoneme_or_boundary_ground_truth"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--voicing", type=Path, required=True)
    parser.add_argument("--validation-audio", type=Path, required=True)
    parser.add_argument("--validation-manual", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records = json.loads(args.base.read_text())
    for record in records:
        for key in ("audio", "annotation"):
            if digest(Path(record[key])) != record[key + "_sha256"]:
                raise ValueError(f"Frozen original changed: {record[key]}")
    info = sf.info(args.validation_audio)
    audio_hash = digest(args.validation_audio)
    manual = json.loads(args.validation_manual.read_text())
    if audio_hash in {r["audio_sha256"] for r in records}:
        raise ValueError("New validation must not already be in training/base")
    if (manual["dataset"]["audio_sha256"] != audio_hash or manual["sample_rate"] != info.samplerate
            or abs(manual["duration"] - info.duration) > 1 / info.samplerate):
        raise ValueError("New validation manual refers to different audio")
    # Explicit user declaration overrides the editor's draft flag; keep a frozen copy.
    manual_path = args.output.resolve() / "validation.manual.annotation.json"
    manual_path.write_bytes(args.validation_manual.read_bytes())
    records.append({"id": audio_hash, "audio": str(args.validation_audio.resolve()),
        "audio_sha256": audio_hash, "annotation": str(manual_path), "annotation_sha256": digest(manual_path),
        "seconds": info.duration, "sample_rate": info.samplerate, "notes": manual["notes"],
        "split": "validation", "group": "clip:" + audio_hash, "review_authority": "explicit_user_validation",
        "validation_cohort": "low_male_vox243"})
    for record in records:
        if record["split"] == "validation": record.setdefault("validation_cohort", "legacy")
    history = []
    for parent in list(records):
        if parent["split"] != "train": continue
        frames = np.asarray(json.loads((args.voicing / f"{parent['id'][:24]}.json").read_text())["yin_frames"])
        times = np.arange(len(frames)) * .01
        inside = np.zeros(len(frames), bool)
        for note in parent["notes"]: inside |= (times >= note["start"]) & (times < note["end"])
        good = (frames[:, 2] > .5) & (frames[:, 1] >= .85) & inside
        if good.sum() < 50: continue
        hz = 440 * 2 ** ((frames[good, 0] - 69) / 12)
        median = float(np.median(hz))
        if median < 180: continue
        shift = max(-12, min(-3, round(12 * math.log2(150 / median))))
        # Avoid routinely pushing the low part below native YIN's usable range.
        while shift < -3 and np.percentile(hz, 20) * 2 ** (shift / 12) < 78: shift += 1
        audio, rate = sf.read(parent["audio"], dtype="float32", always_2d=True)
        converted = librosa.effects.pitch_shift(audio.T, sr=rate, n_steps=shift,
            res_type="soxr_hq", n_fft=2048, hop_length=256).T
        if converted.shape != audio.shape or not np.isfinite(converted).all():
            raise ValueError("Pitch renderer changed length or produced invalid samples")
        converted *= min(1.0, .98 / max(float(np.abs(converted).max()), 1e-8))
        quality = alignment_check(audio, converted, rate,
            [n[k] for n in parent["notes"] for k in ("start", "end")])
        accepted = (quality["envelope_correlation"] >= .9 and quality["reliable_fraction"] >= .8
                    and quality["p95_abs_lag_ms"] is not None and quality["p95_abs_lag_ms"] <= 20)
        history.append({"parent_id": parent["id"], "semitones": shift, "original_median_hz": median,
                        "accepted": accepted, "quality": quality})
        if accepted:
            audio_path = args.output.resolve() / "pitch" / f"{parent['id'][:24]}_down{abs(shift):02}.wav"
            audio_path.parent.mkdir(exist_ok=True)
            sf.write(audio_path, converted, rate, subtype="PCM_24")
            records.append(derivative_record(parent, audio_path, shift, "librosa_phase_vocoder_soxr_hq", quality))
        print(json.dumps(history[-1]), flush=True)
    prepared = args.output / "prepared.json"
    prepared.write_text(json.dumps({"schema": SCHEMA, "base_manifest": str(args.base.resolve()),
        "base_manifest_sha256": digest(args.base), "records": records, "render_history": history,
        "librosa_version": librosa.__version__, "policy": "originals_plus_training_only_lowered_copies"}, indent=2, allow_nan=False))
    read_prepared(prepared)
    print(json.dumps({"records": len(records), "derivatives": sum(h["accepted"] for h in history),
                      "rejected": sum(not h["accepted"] for h in history), "prepared": str(prepared)}), flush=True)


if __name__ == "__main__": main()
