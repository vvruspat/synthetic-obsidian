"""Verify native/PyTorch parity and compare frozen AI drafts against the YIN baseline."""
import argparse
import json
from pathlib import Path
import subprocess

import numpy as np
import soundfile as sf
import torch

from .model import decode, DRAFT_POLICY
from .multiscale import make_model, make_features
from .train import interval_metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--experiment", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--split", choices=["validation", "test"], default="test")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    records = json.loads((args.experiment / "manifest.json").read_text())
    exported = json.loads((args.experiment / "model.json").read_text())
    checkpoint = torch.load(args.experiment / "checkpoint.pt", map_location="cpu", weights_only=True)
    model = make_model(checkpoint["schema"]).eval()
    model.load_state_dict(checkpoint["model_state"])
    model_file = (args.experiment / "model.json").resolve()
    parity = []
    # Compare both source sample rates without using test labels for any tuning.
    by_rate = {}
    for record in sorted(records, key=lambda r: r["seconds"]):
        if record["split"] != "test": by_rate.setdefault(record["sample_rate"], record)
    for rate, record in by_rate.items():
        output = args.output / f"parity-{rate}.json"
        subprocess.run([str(args.runner.resolve()), "--predict", str(model_file), record["audio"], str(output.resolve()), "diagnostics"], check=True)
        native = json.loads(output.read_text())
        audio, sample_rate = sf.read(record["audio"], dtype="float32", always_2d=True)
        x = make_features(audio, sample_rate, checkpoint["schema"], native.get("yin_frames"))
        native_features = np.array(native["features"]).reshape(-1, x.shape[0]).T
        feature_error = float(np.abs(x - native_features).max())
        with torch.inference_mode(): probabilities = model(torch.from_numpy(x).unsqueeze(0)).sigmoid().squeeze(0).numpy()
        probability_error = float(np.abs(probabilities.T - np.array(native["probabilities"])).max())
        expected = decode(probabilities, record["seconds"], tuple(exported["thresholds"]))
        np.testing.assert_allclose(np.array(native["notes"]).reshape(-1, 3)[:, :2],
                                   np.array(expected).reshape(-1, 2), atol=1e-8)
        if feature_error > 0.015 or probability_error > 0.01:
            raise AssertionError(f"Native parity failed: features={feature_error}, probabilities={probability_error}")
        parity.append({"sample_rate": rate, "max_feature_error": feature_error, "max_probability_error": probability_error,
                       "notes": len(expected), "elapsed_ms": native["elapsed_ms"]})
        print(parity[-1], flush=True)
    test = [r for r in records if r["split"] == args.split]
    learned, baseline, timing = [], [], []
    for record in test:
        output = args.output / f"{args.split}-{record['id'][:24]}.json"
        subprocess.run([str(args.runner.resolve()), "--predict", str(model_file), record["audio"], str(output.resolve())], check=True)
        native = json.loads(output.read_text())
        learned.append([(n[0], n[1]) for n in native["notes"]])
        baseline.append([(n[0], n[1]) for n in native["baseline_notes"]])
        timing.append({"source_seconds": record["seconds"], "elapsed_ms": native["elapsed_ms"]})
    report = {"parity": parity, f"{args.split}_frozen_thresholds": interval_metrics(learned, test),
              f"{args.split}_math_baseline": interval_metrics(baseline, test), "native_timing": timing,
              "draft_policy": DRAFT_POLICY,
              f"{args.split}_notes": sum(len(r["notes"]) for r in test)}
    (args.output / "native-report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == "__main__": main()
