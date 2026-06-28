#!/usr/bin/env python3
"""Run Seed-VC for one vocal file across pitch/harmony intervals."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path


DEFAULT_INTERVALS = {
    "identity": 0,
    "up2st": 2,
    "down2st": -2,
    "harmony_minor_third": 3,
    "harmony_major_third": 4,
    "harmony_fifth": 7,
    "harmony_octave": 12,
}


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def run_seed_vc_file(
    source: Path,
    output_root: Path,
    python_bin: str,
    diffusion_steps: int,
    intervals: dict[str, int],
) -> None:
    root = _repo_root()
    seed_vc_dir = root / "third_party" / "seed-vc"
    inference_script = seed_vc_dir / "inference.py"

    if not source.exists():
        raise FileNotFoundError(f"Missing source file: {source}")
    if not inference_script.exists():
        raise FileNotFoundError(f"Missing Seed-VC inference script: {inference_script}")

    output_root.mkdir(parents=True, exist_ok=True)

    for name, semitones in intervals.items():
        output_dir = output_root / name
        output_dir.mkdir(parents=True, exist_ok=True)
        command = [
            python_bin,
            str(inference_script),
            "--source",
            str(source),
            "--target",
            str(source),
            "--output",
            str(output_dir),
            "--diffusion-steps",
            str(diffusion_steps),
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
        print(f"\n=== {name} ({semitones:+d} st) ===")
        subprocess.run(command, cwd=root, check=True)


def _parse_intervals(values: list[str] | None) -> dict[str, int]:
    if not values:
        return DEFAULT_INTERVALS

    intervals: dict[str, int] = {}
    for value in values:
        name, semitones = value.split(":", 1)
        intervals[name] = int(semitones)
    return intervals


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, required=True)
    parser.add_argument("--python-bin", default=sys.executable)
    parser.add_argument("--diffusion-steps", type=int, default=30)
    parser.add_argument(
        "--interval",
        action="append",
        help="Optional interval in name:semitones form. Repeatable.",
    )
    args = parser.parse_args()

    run_seed_vc_file(
        source=args.source.expanduser().resolve(),
        output_root=args.output_root.resolve(),
        python_bin=args.python_bin,
        diffusion_steps=args.diffusion_steps,
        intervals=_parse_intervals(args.interval),
    )


if __name__ == "__main__":
    main()
