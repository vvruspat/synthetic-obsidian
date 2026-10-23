"""Validate controlled synthesis separately from inherited human augmentation."""
import hashlib
import json
import math
from pathlib import Path

from .corrections import digest

SCHEMA = "native-note-boundary-controlled-synthesis-v1"
QA_POLICY = "native-yin-f0-and-rms-phrase-alignment-v1"


def pinned_json(path, expected_hash, cache):
    path = Path(path).resolve()
    key = (str(path), expected_hash)
    if key not in cache:
        if digest(path) != expected_hash:
            raise ValueError(f"Synthetic provenance changed: {path}")
        cache[key] = json.loads(path.read_text())
    return cache[key]


def validate_record(record, root, cache):
    if record["split"] != "train" or record.get("review_authority") != "controlled_synthesis_not_human_reviewed":
        raise ValueError("Controlled synthesis must stay synthetic and training-only")
    source = record["synthesis"]
    documents = {name: pinned_json(root/source[name], source[name+"_sha256"], cache)
                 for name in ("plan", "recipe", "qa")}
    plan, recipe, qa = (documents[k] for k in ("plan", "recipe", "qa"))
    if digest(root/source["midi"]) != source["midi_sha256"]:
        raise ValueError("Synthetic MIDI changed")
    if plan.get("schema") != SCHEMA or qa.get("policy") != QA_POLICY or qa.get("accepted") is not True:
        raise ValueError("Unapproved synthesis/QA contract")
    identity = hashlib.sha256(json.dumps(recipe, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
    matching = [r for r in plan["records"] if r["id"] == record["id"]]
    if identity != record["id"] or len(matching) != 1 or matching[0]["recipe_sha256"] != source["recipe_sha256"]:
        raise ValueError("Synthetic recipe identity differs")
    if recipe["bank"] != source["bank"] or recipe["bank_files_sha256"] != plan["banks"][recipe["bank"]]["files_sha256"]:
        raise ValueError("Synthetic voice provenance differs")
    annotation = json.loads(Path(record["annotation"]).read_text())
    if annotation.get("dataset", {}).get("review_status") != "synthetic":
        raise ValueError("Synthetic labels cannot be presented as human-reviewed")
    if record.get("ignore_intervals", []) != qa["ignore_intervals"] or record.get("ignore_intervals", []) != annotation["training"]["ignore_intervals"]:
        raise ValueError("Synthetic uncertainty masks differ")
    for start, end in record.get("ignore_intervals", []):
        if not math.isfinite(start+end) or not 0 <= start < end <= record["seconds"]:
            raise ValueError("Invalid synthetic uncertainty mask")
    unit = recipe["score"]["bpm"] * 8
    if len(recipe["notes"]) != len(record["notes"]):
        raise ValueError("Synthetic note count changed")
    def adjusted(time, kind):
        points = [p for p in qa["aligned_boundaries"] if p["kind"] in (kind, "legato")
                  and abs(p["nominal"]-time) < 1e-8]
        return points[0]["aligned"] if points else time
    for i, (nominal, actual) in enumerate(zip(recipe["notes"], record["notes"], strict=True)):
        start = adjusted(nominal["position"]/unit, "phrase_onset")
        end = adjusted((nominal["position"]+nominal["duration"])/unit, "phrase_offset")
        if (actual["id"] != f"n{i+1:04}" or abs(actual["start"]-start) > 1e-8
                or abs(actual["end"]-end) > 1e-8 or "synthetic" not in actual.get("flags", [])):
            raise ValueError("Synthetic boundaries disagree with the frozen acoustic alignment")


def merge(base_path, synthesis_path, output):
    from .augmentation import read_prepared
    base_path, synthesis_path, output = (Path(p).resolve() for p in (base_path, synthesis_path, output))
    if output.exists():
        raise ValueError("Choose a new prepared output; never overwrite frozen training inputs")
    originals = read_prepared(base_path)
    document = json.loads(base_path.read_text())
    synthesis = json.loads(synthesis_path.read_text())
    if synthesis.get("schema") != SCHEMA:
        raise ValueError("Unknown controlled synthesis manifest")
    document["base_manifest"] = str((base_path.parent/document["base_manifest"]).resolve())
    # Every path remains valid when the combined file lives outside either dataset.
    for r in originals:
        if "alignment" in r.get("augmentation", {}):
            r["augmentation"]["alignment"] = str((base_path.parent/r["augmentation"]["alignment"]).resolve())
        for name in ("plan", "recipe", "qa", "midi"):
            if name in r.get("synthesis", {}):
                r["synthesis"][name] = str((base_path.parent/r["synthesis"][name]).resolve())
    identities = {r["id"] for r in originals}
    for r in synthesis["records"]:
        if r["id"] in identities:
            raise ValueError("A synthetic recipe collides with a frozen record")
        identities.add(r["id"])
        for name in ("audio", "annotation"):
            r[name] = str((synthesis_path.parent/r[name]).resolve())
        for name in ("plan", "recipe", "qa", "midi"):
            r["synthesis"][name] = str((synthesis_path.parent/r["synthesis"][name]).resolve())
    document["records"] = originals+synthesis["records"]
    document["controlled_synthesis_source"] = {"path": str(synthesis_path), "sha256": digest(synthesis_path)}
    output.write_text(json.dumps(document, indent=2, ensure_ascii=False, allow_nan=False)+"\n")
    result = read_prepared(output)
    print(json.dumps({"records": len(result), "new_synthetic": len(synthesis["records"]),
                      "previous_records_preserved": len(originals), "output": str(output)}))


if __name__ == "__main__":
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base", type=Path, required=True)
    parser.add_argument("--synthesis", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    merge(args.base, args.synthesis, args.output)
