"""Score all confidently nonzero isolated-stem frames, including phoneme edges."""
import argparse
import json
from pathlib import Path
import numpy as np

from .pipeline import frame_metrics


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--statistics", type=Path, required=True)
    parser.add_argument("--training", type=Path, required=True)
    args = parser.parse_args()
    manifest = json.loads(args.manifest.read_text())
    stats = np.load(args.statistics)
    predictions = np.load(args.training / "heldout-predictions.npz")
    training = json.loads((args.training / "report.json").read_text())
    report = {}
    for role in ("validation", "test"):
        truths = []
        for record in (r for r in manifest["records"] if r["split"] == role):
            lo = round(record["start"] * 100)
            hi = lo + len(np.load(record["targets"]))
            ess = stats["ess_rms"][lo:hi]
            target = np.full(len(ess), np.nan, np.float32)
            target[ess <= 1e-7] = 0
            target[ess > 1e-5] = 1
            target[:66] = np.nan
            target[-66:] = np.nan
            truths.append(target)
        expected = np.concatenate(truths)
        probability = predictions[f"{role}_probabilities"]
        if len(expected) != len(probability):
            raise ValueError("Heldout frame sequences differ")
        report[role] = {mode: frame_metrics(probability, expected, training[f"{mode}_threshold"])
                        for mode in ("balanced", "conservative")}
    report["scope"] = "Full isolated-stem support: RMS >1e-5 positive, <=1e-7 negative; 660 ms block guards. Quiet fades remain ambiguous; no phoneme-edge guards."
    (args.training / "full-stem-evaluation.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
