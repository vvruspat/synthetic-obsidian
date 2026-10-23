"""Compare frozen native proposals on the same human-corrected clips."""
import argparse
import json
from pathlib import Path

from .train import interval_metrics
from .model import connected_boundaries


def fragmentation(predictions, examples):
    fragmented, extra = 0, 0
    for intervals, example in zip(predictions, examples, strict=True):
        for note in example["notes"]:
            # Ignore small boundary deviations into a neighbouring reference note.
            left, right = note["start"] + 0.03, note["end"] - 0.03
            count = sum(min(end, right) - max(start, left) > 0.01 for start, end in intervals)
            fragmented += count > 1
            extra += max(0, count - 1)
    return {"fragmented_reference_notes": fragmented, "extra_fragments": extra}



def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--baseline-prefix", default="v3-", help="Filename prefix of saved baseline drafts; empty for current evaluate_native outputs")
    parser.add_argument("--candidate", type=Path, required=True)
    parser.add_argument("--split", choices=["validation", "test"], required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    records = [r for r in json.loads(args.manifest.read_text()) if r["split"] == args.split]
    predictions = [[], []]
    clips = []
    for r in records:
        old = json.loads((args.baseline / f"{args.baseline_prefix}{args.split}-{r['id'][:24]}.json").read_text())
        new = json.loads((args.candidate / f"{args.split}-{r['id'][:24]}.json").read_text())
        intervals = [[(n[0], n[1]) for n in result["notes"]] for result in (old, new)]
        for i in range(2): predictions[i].append(intervals[i])
        clips.append({"id": r["id"], "audio": r["audio"],
                      "baseline": {**interval_metrics([intervals[0]], [r]), **fragmentation([intervals[0]], [r]),
                                   "connected_boundaries": connected_boundaries([intervals[0]], [r])},
                      "candidate": {**interval_metrics([intervals[1]], [r]), **fragmentation([intervals[1]], [r]),
                                    "connected_boundaries": connected_boundaries([intervals[1]], [r])}})
    report = {"split": args.split, "clips": len(records), "notes": sum(len(r["notes"]) for r in records),
              "baseline": {**interval_metrics(predictions[0], records), **fragmentation(predictions[0], records),
                           "connected_boundaries": connected_boundaries(predictions[0], records)},
              "candidate": {**interval_metrics(predictions[1], records), **fragmentation(predictions[1], records),
                            "connected_boundaries": connected_boundaries(predictions[1], records)},
              "per_clip": clips, "scope": "Whole clips; related fragments may cross splits. No phoneme labels."}
    args.output.write_text(json.dumps(report, indent=2))
    print(json.dumps({k: v for k, v in report.items() if k != "per_clip"}, indent=2))


if __name__ == "__main__": main()
