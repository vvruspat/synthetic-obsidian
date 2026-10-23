"""Export the frozen classifier to the standalone JUCE runner's array format."""
import argparse
import json
from pathlib import Path
import librosa
import numpy as np
from .pipeline import digest
from .predict import load_model


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    _, _, _, source = load_model(args.source)
    result = {k: v for k, v in source.items() if k != "state"}
    result["schema"] = "synthetic-obsidian-sibilant-native-v1"
    result["python_model_sha256"] = digest(args.source)
    result["mel_filters"] = librosa.filters.mel(sr=24000, n_fft=1024, n_mels=80, fmin=50, fmax=11800).reshape(-1).tolist()
    result["layers"] = {}
    for name, values in source["state"].items():
        value = np.asarray(values, dtype=np.float32)
        if value.ndim == 3:
            value = value.transpose(0, 2, 1)
        result["layers"][name] = value.reshape(-1).tolist()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, separators=(",", ":")))
    print(source["model_id"], digest(args.output))


if __name__ == "__main__":
    main()
