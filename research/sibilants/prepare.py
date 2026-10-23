from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import numpy as np
import soundfile as sf

from .pipeline import CONTEXT_FRAMES, LABEL_EDGE_GUARD, digest, features, labels, pair_statistics


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--raw", type=Path, required=True)
    parser.add_argument("--ess", type=Path, required=True)
    parser.add_argument("--statistics", type=Path, help="Optional verified cache; otherwise derive labels directly from the pair")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    a, b = sf.info(args.raw), sf.info(args.ess)
    if (a.frames, a.samplerate, a.channels) != (b.frames, b.samplerate, b.channels):
        raise ValueError("Aligned source pair has different dimensions")
    pins = {"raw": {"path": str(args.raw.resolve()), "sha256": digest(args.raw)},
            "ess": {"path": str(args.ess.resolve()), "sha256": digest(args.ess)}}
    (args.output / "input-pins.json").write_text(json.dumps(pins, indent=2))
    cache_path = args.statistics or args.output / "frame-statistics.npz"
    stats = dict(np.load(cache_path)) if cache_path.exists() else {}
    if any(str(stats.get(key + "_sha256", "")) != pins[key]["sha256"] for key in ("raw", "ess")):
        stats = pair_statistics(args.raw, args.ess)
        stats.update({key + "_sha256": np.array(pins[key]["sha256"]) for key in ("raw", "ess")})
        np.savez_compressed(cache_path, **stats)
    expected = labels(stats["raw_rms"], stats["ess_rms"])
    if len(expected) != int(np.ceil(a.frames * 100 / a.samplerate)):
        raise ValueError("Source audio and label frame count differ")
    records = []
    block_frames = 12000
    total = len(expected)
    block_ids = list(range((total + block_frames - 1) // block_frames))
    ordered = sorted(block_ids, key=lambda i: hashlib.sha256(f"sibilants-v1-1729-{i}".encode()).digest())
    held = max(1, round(len(ordered) * 0.1))
    roles = {i: "test" if i in ordered[:held] else "validation" if i in ordered[held:2*held] else "train" for i in block_ids}
    with sf.SoundFile(args.raw) as audio:
        for i in block_ids:
            lo, hi = i * block_frames, min(total, (i + 1) * block_frames)
            audio.seek(round(lo * a.samplerate / 100))
            wave = audio.read(min(round((hi - lo) * a.samplerate / 100), a.frames - audio.tell()), dtype="float32", always_2d=True)
            feature_path = args.output / f"block-{i:03d}.features.npy"
            target_path = args.output / f"block-{i:03d}.targets.npy"
            values = features(wave, a.samplerate)
            target = expected[lo:lo + values.shape[1]].copy()
            if len(target) != values.shape[1]:
                raise ValueError("Feature/label frame alignment differs")
            # Each role owns complete two-minute blocks; discard both context margins.
            target[:CONTEXT_FRAMES + 3] = np.nan
            target[-CONTEXT_FRAMES - 3:] = np.nan
            np.save(feature_path, values)
            np.save(target_path, target)
            records.append({"id": i, "split": roles[i], "start": lo / 100, "end": hi / 100,
                            "features": str(feature_path.resolve()), "targets": str(target_path.resolve()),
                            "positive_seconds": float(np.sum(target == 1) / 100),
                            "negative_seconds": float(np.sum(target == 0) / 100),
                            "ignored_seconds": float(np.sum(~np.isfinite(target)) / 100)})
            if i % 5 == 0:
                print(f"Prepared {i + 1}/{len(block_ids)} time blocks", flush=True)
    report = {"input_pair": pins, "source_labels": "User-confirmed complete isolated sibilant stem; uncertain fades/edges ignored.",
              "label_rule": {"minimum_ess_rms": 1e-5, "minimum_ess_to_raw_rms": 0.001,
                             "silence_ess_rms": 1e-7, "edge_guard_frames": LABEL_EDGE_GUARD},
              "scope": "Disjoint 120-second time blocks, guarded by 660 ms. Singers/songs may occur in multiple roles.",
              "sample_rate": a.samplerate, "duration_seconds": a.duration, "records": records}
    for role in ("train", "validation", "test"):
        subset = [r for r in records if r["split"] == role]
        report[role] = {"blocks": len(subset), **{key: round(sum(r[key] for r in subset), 2)
                         for key in ("positive_seconds", "negative_seconds", "ignored_seconds")}}
    (args.output / "manifest.json").write_text(json.dumps(report, indent=2))
    print(json.dumps({role: report[role] for role in ("train", "validation", "test")}, indent=2), flush=True)


if __name__ == "__main__":
    main()
