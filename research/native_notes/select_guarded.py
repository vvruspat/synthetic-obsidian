"""Select a trained epoch on validation while preserving connected-note recall."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

import numpy as np
import soundfile as sf
import torch

from .multiscale import make_model, make_features
from .model import DRAFT_POLICY
from .train import predict, metrics, calibrate, selection_score, export, digest


def calibrated_cohorts(probabilities, examples, floors, extended_thresholds=False):
    """Protect each validation cohort: gains on one cannot conceal lost transitions on another."""
    groups = {name: [i for i, r in enumerate(examples) if r.get("validation_cohort", "legacy") == name]
              for name in floors}
    best = None
    boundary_grid = (.2, .35, .5, .65, .8, .85, .9, .95) if extended_thresholds else (.2, .35, .5, .65, .8)
    activity_grid = (.35, .5, .65, .8, .9) if extended_thresholds else (.35, .5, .65)
    for onset in boundary_grid:
        for offset in boundary_grid:
            for activity in activity_grid:
                thresholds = (onset, offset, activity)
                evaluations = {name: metrics([probabilities[i] for i in ids], [examples[i] for i in ids], thresholds)
                               for name, ids in groups.items()}
                if any(evaluations[name]["connected_boundaries"][key] < floor[key]
                       for name, floor in floors.items() for key in ("onsets_matched", "offsets_matched")):
                    continue
                score = sum(selection_score(e) for e in evaluations.values()) / len(evaluations)
                if best is None or score > best[0]: best = (score, thresholds, evaluations)
    return best


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--experiment", type=Path, required=True)
    parser.add_argument("--baseline", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--model-id", default="sasha-notes-v7")
    parser.add_argument("--name", default="Sasha notes v7 — relative pitch + corrections")
    parser.add_argument("--cohort-guards", action="store_true", help="Preserve transition recall separately per cohort; equal cohort mean F1")
    parser.add_argument("--extended-thresholds", action="store_true", help="Include higher confidence thresholds after fine-tuning; requires --cohort-guards")
    parser.add_argument("--partial-review", type=Path, help="Pinned, explicitly reviewed audio prefix for development selection only")
    parser.add_argument("--native-runner", type=Path, help="Required for native YIN on --partial-review")
    args = parser.parse_args()
    if args.extended_thresholds and not args.cohort_guards:
        parser.error("--extended-thresholds requires --cohort-guards")
    if args.partial_review and (not args.native_runner or not args.cohort_guards):
        parser.error("--partial-review requires --native-runner and --cohort-guards")
    if args.output.exists(): raise ValueError("Use a new output directory for a frozen selection")
    report = json.loads((args.experiment / "report.json").read_text())
    baseline_export = json.loads((args.baseline / "model.json").read_text())
    baseline_checkpoint = torch.load(args.baseline / "checkpoint.pt", weights_only=True, map_location="cpu")
    records = json.loads((args.experiment / "manifest.json").read_text())
    schema = report["feature_schema"]
    validation = []
    for record in records:
        if record["split"] != "validation": continue
        if digest(Path(record["audio"])) != record["audio_sha256"]: raise ValueError("Validation audio changed")
        diagnostics = json.loads((args.experiment / "voicing" / f"{record['id'][:24]}.json").read_text())
        audio, rate = sf.read(record["audio"], dtype="float32", always_2d=True)
        record["features"] = make_features(audio, rate, schema, diagnostics["yin_frames"])
        record["voiced_times"] = np.array(diagnostics["voiced_times"], dtype=np.float64)
        validation.append(record)
    if args.partial_review:
        from .partial_review import load
        extra = load(args.partial_review, schema, args.native_runner, args.experiment / "partial-review-native.json")
        if any(r["id"] == extra["id"] or r["audio_sha256"] == extra["audio_sha256"] for r in records):
            raise ValueError("Partial-review control already appears in the training manifest")
        validation.append(extra)
    torch.set_num_threads(4)
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    baseline = make_model(baseline_checkpoint["schema"]).to(device)
    baseline.load_state_dict(baseline_checkpoint["model_state"])
    baseline_features = baseline.input_projection[0].in_channels
    if baseline_features > validation[0]["features"].shape[0]: raise ValueError("Baseline needs unavailable inputs")
    baseline_examples = [{**r, "features": r["features"][:baseline_features]} for r in validation]
    baseline_probabilities = predict(baseline, baseline_examples, device)
    baseline_evaluation = metrics(baseline_probabilities, baseline_examples, tuple(baseline_export["thresholds"]))
    floor = baseline_evaluation["connected_boundaries"]
    cohorts = sorted({r.get("validation_cohort", "legacy") for r in validation})
    baseline_cohorts = {name: metrics([p for p, r in zip(baseline_probabilities, validation) if r.get("validation_cohort", "legacy") == name],
                                    [r for r in validation if r.get("validation_cohort", "legacy") == name], tuple(baseline_export["thresholds"]))
                        for name in cohorts}
    cohort_floors = {name: e["connected_boundaries"] for name, e in baseline_cohorts.items()}
    model = make_model(schema).to(device)
    history = []
    best = None
    for path in sorted((args.experiment / "epoch-checkpoints").glob("epoch-*.pt")):
        checkpoint = torch.load(path, weights_only=True, map_location="cpu")
        model.load_state_dict(checkpoint["model_state"])
        probabilities = predict(model, validation, device)
        cohort_evaluations = None
        if args.cohort_guards:
            candidate = calibrated_cohorts(probabilities, validation, cohort_floors, args.extended_thresholds)
            score, thresholds, cohort_evaluations = candidate if candidate else (None, None, None)
            evaluation = metrics(probabilities, validation, thresholds) if thresholds else None
        else:
            thresholds, evaluation = calibrate(probabilities, validation, floor)
            score = selection_score(evaluation) if evaluation else None
        history.append({"epoch": checkpoint["training"]["epoch"], "thresholds": thresholds,
                        "score": score, "evaluation": evaluation, "cohorts": cohort_evaluations})
        if score is not None and (best is None or score > best[0]):
            best = (score, path, thresholds, evaluation)
        print(json.dumps({"epoch": checkpoint["training"]["epoch"], "score": score,
                          "best_epoch": int(best[1].stem.split("-")[-1]) if best else None}), flush=True)
    if best is None: raise ValueError("No trained candidate preserves the baseline transition counts")
    score, path, thresholds, evaluation = best
    checkpoint = torch.load(path, weights_only=True, map_location="cpu")
    state = checkpoint["model_state"]
    model.load_state_dict(state)
    final_report = {**report, "best_epoch": checkpoint["training"]["epoch"], "thresholds": thresholds,
                    "draft_policy": DRAFT_POLICY,
                    "partial_review_selection": extra["partial_review"] if args.partial_review else None,
                    "extended_threshold_search": args.extended_thresholds,
                    "selection_policy": "equal_validation_cohort_mean_f1_with_per_cohort_transition_recall_floor" if args.cohort_guards else "validation_mean_f1_with_baseline_transition_recall_floor",
                    "unconstrained_best_epoch": report["best_epoch"], "validation_calibrated": evaluation,
                    "baseline_model_id": baseline_export["model_id"], "baseline_validation": baseline_evaluation,
                    "baseline_validation_cohorts": baseline_cohorts,
                    "selected_validation_cohorts": next(h["cohorts"] for h in history if h["epoch"] == checkpoint["training"]["epoch"]),
                    "baseline_validation_score": (sum(selection_score(e) for e in baseline_cohorts.values()) / len(baseline_cohorts)
                                                  if args.cohort_guards else selection_score(baseline_evaluation)),
                    "selected_validation_score": score,
                    "improves_baseline_selection_score": score > (sum(selection_score(e) for e in baseline_cohorts.values()) / len(baseline_cohorts)
                                                                 if args.cohort_guards else selection_score(baseline_evaluation)),
                    "connected_floor": {key: floor[key] for key in ("onsets_matched", "offsets_matched")},
                    "selection_history": history, "test_frozen_thresholds": None,
                    "selected_epoch_checkpoint_sha256": digest(path)}
    identity = hashlib.sha256(digest(path).encode())
    identity.update(json.dumps(thresholds).encode())
    model_id = args.model_id + "-" + identity.hexdigest()[:12]
    args.output.mkdir(parents=True)
    export(model, thresholds, args.output / "model.json", model_id, final_report, args.name)
    torch.save({"schema": schema, "model_state": state, "training": final_report}, args.output / "checkpoint.pt")
    (args.output / "report.json").write_text(json.dumps(final_report, indent=2, allow_nan=False))
    shutil.copyfile(args.experiment / "manifest.json", args.output / "manifest.json")
    print(json.dumps({"selected_epoch": final_report["best_epoch"], "thresholds": thresholds,
                      "baseline_score": final_report["baseline_validation_score"], "selected_score": score,
                      "model_id": model_id, "evaluation": evaluation}), flush=True)


if __name__ == "__main__": main()
