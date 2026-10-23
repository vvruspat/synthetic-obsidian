"""Explicitly scoped human review for development selection, never training."""
import json
from pathlib import Path
import subprocess

import numpy as np
import soundfile as sf

from .corrections import digest
from .multiscale import make_features


def reviewed_prefix(notes, seconds, start, end):
    if start != 0 or not 0 < end <= seconds:
        raise ValueError("Only an explicitly reviewed prefix is supported")
    if any(n["start"] < end < n["end"] for n in notes):
        raise ValueError("A note crosses the review limit; its end is not reviewed")
    return [n for n in notes if 0 <= n["start"] < end and n["end"] <= end]


def load(path, schema, runner, diagnostics_path):
    spec = json.loads(Path(path).read_text())
    if spec.get("role") != "validation_only_for_selection_not_training":
        raise ValueError("Partial review must be reserved for model selection")
    audio_path, manual_path = Path(spec["audio"]), Path(spec["manual"])
    if digest(audio_path) != spec["audio_sha256"] or digest(manual_path) != spec["manual_sha256"]:
        raise ValueError("Partial review inputs changed")
    document = json.loads(manual_path.read_text())
    audio, rate = sf.read(audio_path, dtype="float32", always_2d=True)
    if (document["dataset"]["audio_sha256"] != spec["audio_sha256"] or rate != document["sample_rate"]
            or abs(len(audio)/rate - document["duration"]) > 1/rate):
        raise ValueError("Partial review refers to different audio")
    notes = reviewed_prefix(document["notes"], document["duration"], spec["reviewed_start"], spec["reviewed_end"])
    diagnostics_path = Path(diagnostics_path).resolve()
    if not diagnostics_path.exists():
        subprocess.run([str(Path(runner).resolve()), "--predict", "math", str(audio_path),
                        str(diagnostics_path), "diagnostics"], check=True)
    diagnostics = json.loads(diagnostics_path.read_text())
    return {"id": document["dataset"]["source_sha256"], "audio": str(audio_path),
            "audio_sha256": spec["audio_sha256"], "notes": notes, "seconds": spec["reviewed_end"],
            "source_seconds": document["duration"], "features": make_features(audio, rate, schema, diagnostics["yin_frames"]),
            "voiced_times": np.array([t for t in diagnostics["voiced_times"] if t < spec["reviewed_end"]]),
            "validation_cohort": spec["name"], "partial_review": spec}
