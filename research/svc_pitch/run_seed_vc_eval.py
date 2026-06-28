#!/usr/bin/env python3
"""Run Seed-VC on the fixed SVC pitch evaluation set.

This wrapper keeps Seed-VC invocation reproducible and separate from the older
DDSP experiments. It expects Seed-VC to be cloned into `third_party/seed-vc`.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path
from typing import Any

import yaml


def _load_yaml(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return yaml.safe_load(f)


def _load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def _resolve(base: Path, value: str) -> Path:
    path = Path(value)
    if path.is_absolute():
        return path
    return (base / path).resolve()


def run_seed_vc_eval(config_path: Path, python_bin: str) -> None:
    config = _load_yaml(config_path)
    config_dir = config_path.parent
    eval_dir = _resolve(config_dir, config["eval_dir"])
    manifest = _load_json(eval_dir / "manifest.json")

    backend = config["backends"]["seed_vc"]
    repo_dir = _resolve(config_dir, backend["repo_dir"])
    output_root = _resolve(config_dir, backend["output_dir"])
    output_root.mkdir(parents=True, exist_ok=True)

    inference_script = repo_dir / "inference.py"
    if not inference_script.exists():
        raise FileNotFoundError(f"Seed-VC inference script not found: {inference_script}")

    # For the first quality check we use each clip as its own reference. That
    # tests pitch edit quality without adding cross-speaker conversion as a
    # confounding variable.
    for clip in manifest["clips"]:
        clip_name = clip["name"]
        source = Path(clip["audio"])
        target = source

        for output_name, semitones in manifest["outputs"].items():
            output_dir = output_root / clip_name / output_name
            output_dir.mkdir(parents=True, exist_ok=True)

            command = [
                python_bin,
                str(inference_script),
                "--source",
                str(source),
                "--target",
                str(target),
                "--output",
                str(output_dir),
                "--diffusion-steps",
                "30",
                "--length-adjust",
                "1.0",
                "--inference-cfg-rate",
                "0.7",
                "--f0-condition",
                "True",
                "--auto-f0-adjust",
                "False",
                "--semi-tone-shift",
                str(semitones),
                "--fp16",
                "False",
            ]
            print("Running:", " ".join(command))
            subprocess.run(command, cwd=repo_dir, check=True)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--config",
        type=Path,
        default=Path(__file__).with_name("spike_config.yaml"),
    )
    parser.add_argument("--python-bin", default=sys.executable)
    args = parser.parse_args()
    run_seed_vc_eval(args.config.resolve(), args.python_bin)


if __name__ == "__main__":
    main()
