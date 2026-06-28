#!/usr/bin/env python3
"""Prepare the fixed listening set for SVC/RVC pitch experiments."""

from __future__ import annotations

import argparse
import json
import shutil
from pathlib import Path
from typing import Any

import yaml


def _load_config(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return yaml.safe_load(f)


def _resolve(base: Path, value: str) -> Path:
    path = Path(value)
    if path.is_absolute():
        return path
    return (base / path).resolve()


def prepare_eval_set(config_path: Path) -> Path:
    config = _load_config(config_path)
    config_dir = config_path.parent
    source_dir = _resolve(config_dir, config["source_dir"])
    eval_dir = _resolve(config_dir, config["eval_dir"])
    input_dir = eval_dir / "input"
    input_dir.mkdir(parents=True, exist_ok=True)

    manifest: dict[str, Any] = {
        "sample_rate": int(config["sample_rate"]),
        "clips": [],
        "outputs": config["outputs"],
    }

    for clip in config["clips"]:
        src = source_dir / f"{clip}_original.wav"
        if not src.exists():
            raise FileNotFoundError(f"Missing source clip: {src}")

        dst = input_dir / f"{clip}.wav"
        shutil.copy2(src, dst)
        manifest["clips"].append(
            {
                "name": clip,
                "audio": str(dst),
            }
        )

    manifest_path = eval_dir / "manifest.json"
    with manifest_path.open("w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")

    return manifest_path


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--config",
        type=Path,
        default=Path(__file__).with_name("spike_config.yaml"),
    )
    args = parser.parse_args()
    manifest_path = prepare_eval_set(args.config.resolve())
    print(f"Wrote {manifest_path}")


if __name__ == "__main__":
    main()
