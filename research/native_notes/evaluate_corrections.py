"""Compare a native candidate against pinned proposals on human training corrections."""
import argparse
import json
from pathlib import Path
import subprocess

from .corrections import add_pairs, digest
from .compare import fragmentation
from .train import interval_metrics


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--runner", type=Path, required=True)
    parser.add_argument("--model", type=Path, required=True)
    parser.add_argument("--pairs", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    records = []
    add_pairs(records, args.pairs)  # Includes hashes, audio identity and interval validation.
    pairs = json.loads(args.pairs.read_text())
    args.output.mkdir(parents=True, exist_ok=True)
    model = json.loads(args.model.read_text())
    clips = []
    for record in records:
        pair = next(p for p in pairs if p["audio_sha256"] == record["audio_sha256"])
        proposal = json.loads(Path(pair["proposal"]).read_text())
        output = args.output / f"{record['id'][:24]}.json"
        subprocess.run([str(args.runner.resolve()), "--predict", str(args.model.resolve()), record["audio"],
                        str(output.resolve()), "diagnostics"], check=True)
        native = json.loads(output.read_text())
        before = [(n["start"], n["end"]) for n in proposal["notes"]]
        after = [tuple(n[:2]) for n in native["notes"]]
        references = [record]
        def evaluation(intervals):
            return {"intervals": len(intervals), **interval_metrics([intervals], references),
                    **fragmentation([intervals], references)}
        original_ids = {n["id"] for n in proposal["notes"]}
        def nearest_delta(intervals, time):
            return 1000 * min(abs(s - time) for s, _ in intervals) if intervals else None
        added = []
        for note in record["notes"]:
            if note["id"] not in original_ids:
                time = note["start"]
                added.append({"time": time,
                              "before_nearest_onset_delta_ms": nearest_delta(before, time),
                              "after_nearest_onset_delta_ms": nearest_delta(after, time)})
        clips.append({"source_id": record["id"], "audio": record["audio"],
                      "manual_sha256": record["annotation_sha256"], "before": evaluation(before),
                      "candidate": evaluation(after), "manually_added_intervals": added})
    report = {"model_id": model["model_id"], "model_sha256": digest(args.model),
              "scope": "Training correction pairs: measures learning the edits, not unseen-data accuracy.",
              "clips": clips}
    (args.output / "report.json").write_text(json.dumps(report, indent=2, allow_nan=False))
    print(json.dumps(report, indent=2, allow_nan=False))


if __name__ == "__main__": main()
