"""Compare exported JUCE features/probabilities with the frozen PyTorch model."""
import argparse
import json
from pathlib import Path
import subprocess
import numpy as np
import soundfile as sf
import torch
from .pipeline import features
from .predict import load_model
from .train import probabilities


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--native-model", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    torch.set_num_threads(2)
    model, mean, std, contract = load_model(args.model)
    report = []
    for sr in (16000, 24000, 44100, 48000, 96000):
        rng = np.random.default_rng(1729)
        time = np.arange(round(sr * 1.731)) / sr
        mono = (.13 * np.sin(2*np.pi*117*time) + .06*np.sin(2*np.pi*234*time)).astype(np.float32)
        mono[time < .1] = 0
        select = (time > .6) & (time < .92)
        mono[select] = rng.normal(0, .12, int(select.sum()))
        wave = np.stack((mono, -mono*.2), axis=1)
        audio = args.output / f"fixture-{sr}.wav"
        result = args.output / f"fixture-{sr}.json"
        sf.write(audio, wave, sr, subtype="FLOAT")
        subprocess.run([str(args.runner.resolve()), "--sibilants", str(args.native_model.resolve()),
                        str(audio.resolve()), str(result.resolve())], check=True, capture_output=True)
        native = json.loads(result.read_text())
        value = features(wave, sr)
        expected_features = ((value - mean[:, None]) / std[:, None]).T
        observed_features = np.asarray(native["features"]).reshape(-1, 86)
        expected = probabilities(model, value, mean, std, torch.device("cpu"))
        observed = np.asarray(native["probabilities"])
        feature_delta = float(np.max(np.abs(expected_features - observed_features)))
        probability_delta = float(np.max(np.abs(expected - observed)))
        if feature_delta > .01 or probability_delta > .0003:
            raise ValueError(f"Native parity failed at {sr}: features {feature_delta}, probability {probability_delta}")
        report.append({"sample_rate": sr, "max_feature_delta": feature_delta, "max_probability_delta": probability_delta,
                       "decision_mismatches": int(np.sum((observed >= native["threshold"]) != (expected >= contract["conservative_threshold"])))})
    (args.output / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
