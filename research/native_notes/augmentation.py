"""Frozen augmentation inputs: derivatives inherit training labels and roles only."""
from __future__ import annotations

import copy
import hashlib
import json
import math
from pathlib import Path

import numpy as np
import soundfile as sf

from .corrections import digest

SCHEMA = "native-note-boundary-augmentation-manifest-v1"


def boundaries(notes):
    return [(n["id"], float(n["start"]), float(n["end"])) for n in notes]


def read_prepared(path: Path):
    document = json.loads(path.read_text())
    if document.get("schema") != SCHEMA:
        raise ValueError("Unknown prepared dataset schema")
    base_path = (path.parent / document["base_manifest"]).resolve()
    if digest(base_path) != document["base_manifest_sha256"]:
        raise ValueError("Base manifest changed")
    base = {r["id"]: r for r in json.loads(base_path.read_text())}
    records = document["records"]
    for record in records:
        for key in ("audio", "annotation"):
            record[key] = str((path.parent / record[key]).resolve())
    index = {r["id"]: r for r in records}
    if len(index) != len(records) or not set(base) <= set(index):
        raise ValueError("Missing or duplicate source records")
    audio_roles = {}
    synthesis_cache = {}
    for record in records:
        role = record["split"]
        if role not in ("train", "validation", "test"):
            raise ValueError("Unknown dataset role")
        for key in ("audio", "annotation"):
            if digest(Path(record[key])) != record[key + "_sha256"]:
                raise ValueError(f"Prepared input changed: {record[key]}")
        info = sf.info(record["audio"])
        annotation = json.loads(Path(record["annotation"]).read_text())
        if (info.samplerate != record["sample_rate"]
                or abs(info.duration - record["seconds"]) > 1 / info.samplerate
                or annotation["sample_rate"] != info.samplerate
                or abs(annotation["duration"] - info.duration) > 1 / info.samplerate
                or boundaries(annotation["notes"]) != boundaries(record["notes"])):
            raise ValueError("Prepared audio/labels disagree")
        previous, ids = 0.0, set()
        for identity, start, end in boundaries(record["notes"]):
            if (not math.isfinite(start + end) or start < previous - 1e-6
                    or end <= start or end > info.duration + 1e-6 or identity in ids):
                raise ValueError("Invalid prepared intervals")
            previous = end
            ids.add(identity)
        prior_role = audio_roles.setdefault(record["audio_sha256"], role)
        if prior_role != role:
            raise ValueError("Identical audio appears in different roles")
        if record["id"] in base:
            prior = base[record["id"]]
            for key in ("split", "audio_sha256", "annotation_sha256", "notes"):
                if record[key] != prior[key]:
                    raise ValueError("A frozen original was changed")
        elif "augmentation" in record:
            provenance = record["augmentation"]
            parent = index[provenance["parent_id"]]
            if role != "train" or parent["split"] != "train" or parent["id"] not in base:
                raise ValueError("Derivatives may only come from original training clips")
            if (provenance["parent_audio_sha256"] != parent["audio_sha256"]
                    or provenance["parent_annotation_sha256"] != parent["annotation_sha256"]
                    or info.frames != sf.info(parent["audio"]).frames
                    or info.samplerate != parent["sample_rate"]):
                raise ValueError("Derivative no longer matches its pinned parent labels/time grid")
            expected = boundaries(parent["notes"])
            if "alignment" in provenance:
                alignment_path = (path.parent / provenance["alignment"]).resolve()
                if digest(alignment_path) != provenance["alignment_sha256"]:
                    raise ValueError("Alignment map changed")
                mapping = json.loads(alignment_path.read_text())
                src, dst = np.asarray(mapping["source_times"]), np.asarray(mapping["target_times"])
                if (src.ndim != 1 or dst.shape != src.shape or len(src) < 2
                        or not np.isfinite(src).all() or not np.isfinite(dst).all()
                        or np.any(np.diff(src) <= 0) or np.any(np.diff(dst) <= 0)
                        or src[0] < 0 or dst[0] < 0 or src[-1] > parent["seconds"] + .021
                        or dst[-1] > record["seconds"] + .021
                        or mapping["source_sha256"] != parent["audio_sha256"]
                        or mapping["synthetic_sha256"] != record["audio_sha256"]):
                    raise ValueError("Invalid or unrelated alignment map")
                expected = [(identity, float(np.clip(np.interp(start, src, dst), 0, info.duration)),
                             float(np.clip(np.interp(end, src, dst), 0, info.duration)))
                            for identity, start, end in expected]
            if boundaries(record["notes"]) != expected:
                raise ValueError("Derivative no longer matches its pinned parent labels/time map")
            if "reference_id" in provenance and index[provenance["reference_id"]]["split"] != "train":
                raise ValueError("Voice references must also be training-only")
            if "reference_id" in provenance and provenance.get("reference_audio_sha256") != index[provenance["reference_id"]]["audio_sha256"]:
                raise ValueError("Voice reference checksum differs")
            if record.get("ignore_intervals", []) != annotation.get("training", {}).get("ignore_intervals", []):
                raise ValueError("Uncertain-label masks disagree")
            for start, end in record.get("ignore_intervals", []):
                if not math.isfinite(start + end) or not 0 <= start < end <= info.duration:
                    raise ValueError("Invalid uncertain-label mask")
        elif "synthesis" in record:
            from .synthesis_manifest import validate_record
            validate_record(record, path.parent, synthesis_cache)
        elif role != "validation" or record.get("review_authority") != "explicit_user_validation":
            raise ValueError("New originals must be explicitly reserved validation")
    return copy.deepcopy(records)


def mask_uncertain_targets(targets, record, hop_seconds=.01):
    """No loss for synthetic regions whose inherited boundaries could not be aligned reliably."""
    times = np.arange(targets.shape[1]) * hop_seconds
    for start, end in record.get("ignore_intervals", []):
        targets[:, (times >= start) & (times <= end)] = np.nan
    return targets


def derivative_record(parent, audio_path, semitones, method, quality):
    """No stale pitch curves: this dataset supervises human boundaries only."""
    audio_path = audio_path.resolve()
    info = sf.info(audio_path)
    audio_hash = digest(audio_path)
    identity = hashlib.sha256((parent["id"] + method + str(semitones) + audio_hash).encode()).hexdigest()
    notes = [{"id": n["id"], "start": n["start"], "end": n["end"],
              "flags": ["synthetic_inherited_human_boundaries"]} for n in parent["notes"]]
    provenance = {"method": method, "semitones": semitones, "parent_id": parent["id"],
                  "parent_audio_sha256": parent["audio_sha256"],
                  "parent_annotation_sha256": parent["annotation_sha256"], "quality": quality}
    annotation_path = audio_path.with_suffix(".annotation.json")
    annotation_path.write_text(json.dumps({"audio": audio_path.name, "duration": info.duration,
        "sample_rate": info.samplerate, "notes": notes,
        "dataset": {"review_status": "synthetic_inherited", "audio_sha256": audio_hash,
                    "source_sha256": identity}, "augmentation": provenance}, indent=2, allow_nan=False))
    record = {"id": identity, "audio": str(audio_path), "audio_sha256": audio_hash,
              "annotation": str(annotation_path), "annotation_sha256": digest(annotation_path),
              "seconds": info.duration, "sample_rate": info.samplerate, "split": "train",
              "group": parent["group"], "notes": notes, "augmentation": provenance,
              "review_authority": "inherited_human_boundaries_not_manually_reviewed"}
    if "correction_centers" in parent:
        record["correction_centers"] = copy.deepcopy(parent["correction_centers"])
    return record
