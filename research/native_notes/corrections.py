"""Explicit human correction pairs and balanced replay of their boundary errors."""
import hashlib
import json
import math
from pathlib import Path

import soundfile as sf


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def error_centers(reference, proposal, tolerance=0.05):
    centers = {"extra": [], "missed": []}
    for head in ("start", "end"):
        for category, source, target in (("extra", proposal, reference), ("missed", reference, proposal)):
            for note in source:
                time = note[head]
                if not any(abs(other[head] - time) <= tolerance + 1e-9 for other in target):
                    centers[category].append(time)
    # Shared onsets/offsets describe the same neighbourhood; replay once per kind.
    for category, values in centers.items():
        unique = []
        for time in sorted(values):
            if not unique or time - unique[-1] > tolerance + 1e-9:
                unique.append(time)
        centers[category] = unique
    return centers


def add_pairs(records, pairs_path):
    pairs = json.loads(pairs_path.read_text())
    for pair in pairs:
        paths = {key: Path(pair[key]).resolve() for key in ("manual", "proposal", "audio")}
        for key, path in paths.items():
            if digest(path) != pair[key + "_sha256"]:
                raise ValueError(f"Correction input changed: {path}")
        manual, proposal = (json.loads(paths[key].read_text()) for key in ("manual", "proposal"))
        audio_hash = pair["audio_sha256"]
        info = sf.info(paths["audio"])
        for document in (manual, proposal):
            if (document.get("dataset", {}).get("audio_sha256") != audio_hash
                    or document["sample_rate"] != info.samplerate
                    or abs(document["duration"] - info.duration) > 1 / info.samplerate):
                raise ValueError("Correction pair refers to different audio")
            previous, ids = 0.0, set()
            for note in document["notes"]:
                start, end = note["start"], note["end"]
                if (not math.isfinite(start + end) or start < previous - 1e-6
                        or end <= start or end > info.duration + 1e-6 or note["id"] in ids):
                    raise ValueError("Invalid correction intervals")
                previous = end
                ids.add(note["id"])
        identity = manual.get("dataset", {}).get("source_sha256") or audio_hash
        existing = [r for r in records if r["id"] == identity or r["audio_sha256"] == audio_hash]
        if any(r["split"] != "train" for r in existing):
            raise ValueError("A correction pair cannot move validation/test audio into training")
        records[:] = [r for r in records if r not in existing]
        records.append({"id": identity, "audio": str(paths["audio"]), "audio_sha256": audio_hash,
                        "annotation": str(paths["manual"]), "annotation_sha256": pair["manual_sha256"],
                        "seconds": info.duration, "sample_rate": info.samplerate, "split": "train",
                        "group": "clip:" + identity, "review_authority": "explicit_human_correction_pair",
                        "notes": manual["notes"], "correction_centers": error_centers(manual["notes"], proposal["notes"]),
                        "correction_proposal_sha256": pair["proposal_sha256"]})
    records.sort(key=lambda r: r["id"])


def replay_starts(example, frames, repeats):
    centers = example.get("correction_centers", {})
    categories = [centers.get(kind, []) for kind in ("extra", "missed")]
    count = max(map(len, categories))
    starts = []
    # Equal window count per available error type; original dataset windows stay.
    for repeat in range(repeats):
        for values in categories:
            for index in range(count if values else 0):
                center = round(values[index % len(values)] * 100)
                jitter = (-25, 25)[repeat % 2]
                starts.append(max(0, min(example["features"].shape[1] - frames, center - frames // 2 + jitter)))
    return starts
