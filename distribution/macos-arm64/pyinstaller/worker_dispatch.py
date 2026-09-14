#!/usr/bin/env python3
"""Dispatch a frozen Synthetic Obsidian worker to its Python entry point."""

from __future__ import annotations

import os
import runpy
import sys
from pathlib import Path


ENTRYPOINTS = {
    "align_lyrics_torchaudio.py": "align_lyrics_torchaudio",
    "analyze_musical_context.py": "analyze_musical_context",
    "analyze_notes_pyin.py": "analyze_notes_pyin",
    "infer_gtsinger_boundaries.py": "infer_gtsinger_boundaries",
    "backing_vocal_worker.py": "backing_vocal_worker",
    "backing_audio_worker.py": "backing_audio_worker",
    "voice_lora_training_worker.py": "voice_lora_training_worker",
}


def main() -> None:
    if len(sys.argv) < 3 or sys.argv[1] != "--synthetic-entrypoint":
        choices = ", ".join(sorted(ENTRYPOINTS))
        raise SystemExit(f"Expected --synthetic-entrypoint <name>; available: {choices}")

    requested = sys.argv[2]
    module = ENTRYPOINTS.get(requested)
    if module is None:
        raise SystemExit(f"Unknown Synthetic Obsidian worker: {requested}")

    os.environ.setdefault(
        "TORCH_HOME",
        str(Path(sys.executable).resolve().parent / "models" / "torch"),
    )
    sys.argv = [requested, *sys.argv[3:]]
    runpy.run_module(module, run_name="__main__")


if __name__ == "__main__":
    main()
