"""Train/export musical note boundaries from the native annotator's WAV/JSON pairs."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
import random
import subprocess
import time
from pathlib import Path

import numpy as np
import soundfile as sf
import torch
from torch import nn
from torch.utils.data import DataLoader, Dataset, WeightedRandomSampler

from .model import (NoteBoundaryNet, SCHEMA, DILATIONS, HEADS, SAMPLE_RATE, HOP, N_FFT,
                    MELS, features, mel_filters, targets, decode, peaks, matched_events, scores,
                    connected_boundaries, DRAFT_POLICY)
from . import multiscale, voicing, pitch_structure, corrections


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def read_dataset(root: Path, accept_user_reviewed: bool, seed: int, groups: dict | None = None,
                 trusted_manifest: list[dict] | None = None) -> list[dict]:
    records: dict[str, dict] = {}
    trusted = {r["annotation_sha256"] for r in trusted_manifest or []}
    for path in sorted(root.rglob("*.annotation.json")):
        annotation = json.loads(path.read_text())
        metadata = annotation.get("dataset", {})
        annotation_hash = digest(path)
        prior_declaration = annotation_hash in trusted
        if metadata.get("review_status") != "human_reviewed" and not accept_user_reviewed and not prior_declaration:
            continue
        audio = (path.parent / annotation["audio"]).resolve()
        info = sf.info(audio)
        if annotation.get("sample_rate") != info.samplerate or abs(float(annotation["duration"]) - info.duration) > 1 / info.samplerate:
            raise ValueError(f"Audio/annotation metadata differ: {path}")
        audio_hash = digest(audio)
        if metadata.get("audio_sha256") and metadata["audio_sha256"] != audio_hash:
            raise ValueError(f"Audio checksum differs: {path}")
        notes = sorted(annotation.get("notes", []), key=lambda n: n["start"])
        previous = 0.0
        for note in notes:
            start, end = float(note["start"]), float(note["end"])
            if not math.isfinite(start + end) or start < previous - 1e-6 or end <= start or end > info.duration + 1e-6:
                raise ValueError(f"Invalid or overlapping note: {path} {note['id']}")
            previous = end
        identity = metadata.get("source_sha256") or audio_hash
        group = (groups or {}).get(identity[:24]) or metadata.get("session_id") or f"clip:{identity}"
        record = {"id": identity, "audio": str(audio), "annotation": str(path),
                  "audio_sha256": audio_hash, "annotation_sha256": annotation_hash,
                  "seconds": info.duration, "sample_rate": info.samplerate, "group": group,
                  "review_authority": ("human_reviewed_flag" if metadata.get("review_status") == "human_reviewed" else "prior_user_declaration" if prior_declaration else "explicit_user_declaration"),
                  "notes": notes, "singer_id": metadata.get("singer_id", "")}
        # Repeated exports must never appear in different splits. Prefer the latest revision.
        if identity not in records or path.stat().st_mtime_ns > Path(records[identity]["annotation"]).stat().st_mtime_ns:
            records[identity] = record
    output = sorted(records.values(), key=lambda r: r["id"])
    group_ids = sorted(set(record["group"] for record in output))
    if len(group_ids) < 3:
        raise ValueError("Need at least three independent clips/groups for train/validation/test")
    random.Random(seed).shuffle(group_ids)
    held = max(1, round(len(group_ids) * 0.18))
    assignment = {group: "test" if i < held else "validation" if i < held * 2 else "train"
                  for i, group in enumerate(group_ids)}
    # Previously held-out songs stay out of training when new corrected exports arrive.
    frozen = {r["id"]: r["split"] for r in trusted_manifest or []}
    for group in group_ids:
        previous = {frozen[r["id"]] for r in output if r["group"] == group and r["id"] in frozen}
        if len(previous) > 1:
            raise ValueError("New session grouping merges formerly separate splits")
        if previous: assignment[group] = previous.pop()
    for record in output:
        record["split"] = assignment[record["group"]]
    return output


class Windows(Dataset):
    def __init__(self, examples: list[dict], frames: int = 400, context: int = 15, correction_repeats: int = 0):
        self.examples = examples
        self.frames = frames
        self.context = context
        self.index = []
        for i, example in enumerate(examples):
            count = example["features"].shape[1]
            self.index.extend((i, start) for start in range(0, count, frames // 2))
            self.index.extend((i, start) for start in corrections.replay_starts(example, frames, correction_repeats))

    def __len__(self):
        return len(self.index)

    def sampling_weights(self, synthetic_weight, controlled_synthetic_weight=None):
        controlled = synthetic_weight if controlled_synthetic_weight is None else controlled_synthetic_weight
        return [controlled if "synthesis" in self.examples[i] else
                synthetic_weight if "augmentation" in self.examples[i] else 1.0
                for i, _ in self.index]

    def __getitem__(self, index):
        i, begin = self.index[index]
        example = self.examples[i]
        count = example["features"].shape[1]
        start, end = max(0, begin - self.context), min(count, begin + self.frames + self.context)
        left = max(0, self.context - begin)
        right = self.frames + 2 * self.context - (end - start) - left
        x = np.pad(example["features"][:, start:end], ((0, 0), (left, right)))
        y = np.pad(example["targets"][:, start:end], ((0, 0), (left, right)), constant_values=np.nan)
        y[:, :self.context] = np.nan
        y[:, self.context + min(self.frames, count - begin):] = np.nan
        return torch.from_numpy(x), torch.from_numpy(y)


def predict(model, examples, device):
    model.eval()
    with torch.inference_mode():
        return [torch.sigmoid(model(torch.from_numpy(e["features"]).unsqueeze(0).to(device))).squeeze(0).cpu().numpy()
                for e in examples]


def metrics(probabilities, examples, thresholds):
    predictions = []
    for probability, example in zip(probabilities, examples, strict=True):
        intervals = decode(probability, example["seconds"], thresholds)
        predictions.append(intervals)
    return interval_metrics(predictions, examples)


def selection_score(evaluation):
    return sum(evaluation[key]["f1"] for key in ("onset_50ms", "offset_50ms", "note_boundaries")) / 3


def calibrate(probabilities, examples, minimum_connected=None):
    best_score = -1.0
    best_thresholds = (0.5, 0.5, 0.5)
    best_evaluation = None
    for onset in (0.2, 0.35, 0.5, 0.65, 0.8):
        for offset in (0.2, 0.35, 0.5, 0.65, 0.8):
            for activity in (0.35, 0.5, 0.65):
                candidate = (onset, offset, activity)
                evaluation = metrics(probabilities, examples, candidate)
                if minimum_connected is not None:
                    connected = evaluation["connected_boundaries"]
                    if any(connected[key] < minimum_connected[key] for key in ("onsets_matched", "offsets_matched")):
                        continue
                score = selection_score(evaluation)
                if score > best_score:
                    best_score = score
                    best_thresholds, best_evaluation = candidate, evaluation
    return best_thresholds, best_evaluation


def interval_metrics(predictions, examples):
    counts = np.zeros((2, 3), dtype=np.int64)
    note_counts = [0, 0, 0]
    internal_splits = 0
    for intervals, example in zip(predictions, examples, strict=True):
        reference = [(note["start"], note["end"]) for note in example["notes"]]
        internal_splits += sum(any(start + 0.05 < p[0] < end - 0.05 for start, end in reference)
                               for p in intervals)
        for head in range(2):
            counts[head] += matched_events([n[head] for n in reference], [n[head] for n in intervals])
        # Maximal bipartite matching for note boundaries (pitch deliberately excluded).
        edges = [[j for j, r in enumerate(reference) if abs(p[0]-r[0]) <= 0.05 + 1e-9
                  and abs(p[1]-r[1]) <= max(0.05, 0.2*(r[1]-r[0])) + 1e-9] for p in intervals]
        owners = {}
        def match(i, seen):
            for j in edges[i]:
                if j in seen: continue
                seen.add(j)
                if j not in owners or match(owners[j], seen): owners[j] = i; return True
            return False
        true = sum(match(i, set()) for i in range(len(intervals)))
        note_counts = [note_counts[0]+true, note_counts[1]+len(intervals)-true, note_counts[2]+len(reference)-true]
    return {"onset_50ms": scores(tuple(counts[0])), "offset_50ms": scores(tuple(counts[1])),
            "note_boundaries": scores(tuple(note_counts)), "internal_note_splits": internal_splits,
            "connected_boundaries": connected_boundaries(predictions, examples)}


def boundary_loss_weights(expected, interior_weight):
    # A consonant inside a corrected note is still activity, not a new boundary.
    # Leave the soft target's +/-30 ms neighbourhood untouched.
    weights = torch.ones_like(expected)
    interior = (expected[:, 2:3] > 0.5) & (expected[:, :2] < 0.01)
    weights[:, :2] = torch.where(interior, interior_weight, 1.0)
    return weights


def export(model, thresholds, output: Path, model_id: str, report, name: str = "Sasha notes v1"):
    layers = {}
    for key, value in model.state_dict().items():
        # Native matrices are [output, kernel, input], with time-major activations.
        tensor = value.transpose(1, 2) if value.ndim == 3 else value
        layers[key] = tensor.detach().cpu().flatten().tolist()
    document = {"schema": SCHEMA, "model_id": model_id, "name": name,
                "sample_rate": SAMPLE_RATE, "hop_length": HOP, "n_fft": N_FFT, "n_mels": MELS,
                "channels": 64, "dilations": list(DILATIONS), "heads": list(HEADS),
                "feature_contract": "strongest-linear-hann-slaney-clipnorm-v1",
                "mel_filters": mel_filters().flatten().tolist(), "layers": layers,
                "thresholds": list(thresholds), "training": report}
    if isinstance(model, pitch_structure.PitchStructureNoteBoundaryNet):
        document.update(pitch_structure.feature_metadata())
    elif isinstance(model, voicing.VoicingNoteBoundaryNet):
        document.update(voicing.feature_metadata())
    elif isinstance(model, multiscale.MultiScaleNoteBoundaryNet):
        document.update(multiscale.feature_metadata())
    temporary = output.with_suffix(".tmp")
    temporary.write_text(json.dumps(document, separators=(",", ":"), allow_nan=False))
    temporary.replace(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    inputs = parser.add_mutually_exclusive_group(required=True)
    inputs.add_argument("--dataset", type=Path)
    inputs.add_argument("--prepared-manifest", type=Path, help="Pinned originals plus training-only derivatives")
    parser.add_argument("--output", type=Path, required=True)
    initialization = parser.add_mutually_exclusive_group()
    initialization.add_argument("--pretrained", type=Path)
    initialization.add_argument("--resume", type=Path)
    parser.add_argument("--trusted-manifest", type=Path, help="Retain reviewed revisions and frozen splits from a previous experiment")
    parser.add_argument("--name", default="Sasha notes v1")
    parser.add_argument("--model-id", default="sasha-notes-v1")
    parser.add_argument("--groups", type=Path)
    parser.add_argument("--distinct-songs", action="store_true", help="User confirms each clip belongs to a different song")
    parser.add_argument("--accept-user-reviewed", action="store_true")
    parser.add_argument("--epochs", type=int, default=100)
    parser.add_argument("--seed", type=int, default=1701)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--native-runner", type=Path, help="Use the editor's native YIN voicing filter during validation and test")
    parser.add_argument("--learning-rate-scale", type=float, default=1.0, help="Scale both learning rates for conservative fine-tuning")
    parser.add_argument("--skip-test", action="store_true", help="Defer held-out evaluation until candidate selection is finished")
    parser.add_argument("--multiscale", action="store_true", help="32/128-ms windows, noise cues and 950-ms temporal context")
    parser.add_argument("--voicing", action="store_true", help="Add relative pitch/periodicity cues from native YIN; requires --native-runner")
    parser.add_argument("--pitch-structure", action="store_true", help="Add stable pitch centres, edge trends and modulation; warm-start from a v3 checkpoint")
    parser.add_argument("--structure-only", action="store_true", help="Freeze existing v9 weights and learn only the new pitch inputs")
    parser.add_argument("--correction-pairs", type=Path, help="Explicit SHA-256-pinned human correction pairs to add to training")
    parser.add_argument("--correction-repeats", type=int, default=2, help="Balanced extra/missed-boundary replay passes")
    parser.add_argument("--save-epoch-checkpoints", action="store_true", help="Keep evaluated epochs for reproducible diagnostics")
    parser.add_argument("--synthetic-weight", type=float, default=1.0,
                        help="Relative window sampling weight for synthetic derivatives; 1 preserves ordinary shuffle")
    parser.add_argument("--controlled-synthetic-weight", type=float,
                        help="Override sampling weight for controlled MIDI synthesis; default follows --synthetic-weight")
    parser.add_argument("--interior-boundary-weight", type=float, default=1.0,
                        help="Extra penalty for false boundaries inside a human-corrected note")
    args = parser.parse_args()
    if args.structure_only and not args.pitch_structure:
        parser.error("--structure-only requires --pitch-structure")
    if not math.isfinite(args.learning_rate_scale) or args.learning_rate_scale <= 0:
        parser.error("--learning-rate-scale must be finite and positive")
    if not math.isfinite(args.interior_boundary_weight) or args.interior_boundary_weight < 1:
        parser.error("--interior-boundary-weight must be finite and at least 1")
    args.voicing = args.voicing or args.pitch_structure
    if args.voicing and not args.native_runner: parser.error("--voicing requires --native-runner")
    if args.correction_repeats < 0: parser.error("--correction-repeats must be nonnegative")
    if not math.isfinite(args.synthetic_weight) or args.synthetic_weight <= 0:
        parser.error("--synthetic-weight must be finite and positive")
    if args.controlled_synthetic_weight is not None and (not math.isfinite(args.controlled_synthetic_weight) or args.controlled_synthetic_weight <= 0):
        parser.error("--controlled-synthetic-weight must be finite and positive")
    args.multiscale = args.multiscale or args.voicing
    schema = pitch_structure.SCHEMA if args.pitch_structure else voicing.SCHEMA if args.voicing else multiscale.SCHEMA if args.multiscale else SCHEMA
    args.output.mkdir(parents=True, exist_ok=True)
    random.seed(args.seed); np.random.seed(args.seed); torch.manual_seed(args.seed); torch.set_num_threads(4)
    groups = json.loads(args.groups.read_text()) if args.groups else None
    trusted = json.loads(args.trusted_manifest.read_text()) if args.trusted_manifest else None
    if args.prepared_manifest:
        from .augmentation import read_prepared
        if args.correction_pairs or args.groups or args.trusted_manifest or args.accept_user_reviewed:
            parser.error("Prepared inputs already freeze review authority, corrections and roles")
        records = read_prepared(args.prepared_manifest)
    else:
        records = read_dataset(args.dataset.resolve(), args.accept_user_reviewed, args.seed, groups, trusted)
    if args.correction_pairs:
        corrections.add_pairs(records, args.correction_pairs)
    (args.output / "manifest.json").write_text(json.dumps(records, indent=2, ensure_ascii=False, allow_nan=False))
    for record in records:
        audio, rate = sf.read(record["audio"], dtype="float32", always_2d=True)
        native_frames = None
        if args.native_runner:
            voicing_output = args.output / "voicing" / f"{record['id'][:24]}.json"
            voicing_output.parent.mkdir(exist_ok=True)
            subprocess.run([str(args.native_runner.resolve()), "--predict", "math", record["audio"],
                            str(voicing_output.resolve()), "diagnostics"], check=True)
            diagnostics = json.loads(voicing_output.read_text())
            record["voiced_times"] = np.asarray(diagnostics["voiced_times"], dtype=np.float64)
            native_frames = diagnostics.get("yin_frames")
        record["features"] = multiscale.make_features(audio, rate, schema, native_frames)
        record["targets"] = targets(record["notes"], record["features"].shape[1])
        if record.get("ignore_intervals"):
            from .augmentation import mask_uncertain_targets
            mask_uncertain_targets(record["targets"], record)
        print(f"loaded {record['id'][:12]} {record['split']} {record['seconds']:.1f}s {len(record['notes'])} notes", flush=True)
    training = [r for r in records if r["split"] == "train"]
    validation = [r for r in records if r["split"] == "validation"]
    test = [r for r in records if r["split"] == "test"]
    device = torch.device(("mps" if torch.backends.mps.is_available() else "cpu") if args.device == "auto" else args.device)
    model = multiscale.make_model(schema)
    if args.pretrained:
        if args.multiscale: raise ValueError("Initialize multiscale with --resume from a native checkpoint")
        model.initialize_encoder(torch.load(args.pretrained, map_location="cpu", weights_only=True))
    if args.resume:
        checkpoint = torch.load(args.resume, map_location="cpu", weights_only=True)
        if args.pitch_structure and checkpoint.get("schema") == voicing.SCHEMA:
            model.initialize_voicing(checkpoint["model_state"])
        elif args.voicing and not args.pitch_structure and checkpoint.get("schema") == multiscale.SCHEMA:
            model.initialize_multiscale(checkpoint["model_state"])
        elif args.multiscale and not args.voicing and checkpoint.get("schema") == SCHEMA:
            model.initialize_legacy(checkpoint["model_state"])
        elif checkpoint.get("schema") == schema:
            model.load_state_dict(checkpoint["model_state"])
        else: raise ValueError("Resume checkpoint uses a different feature/model contract")
    model.to(device)
    training_windows = Windows(training, context=47 if args.multiscale else 15, correction_repeats=args.correction_repeats)
    sampler = (WeightedRandomSampler(training_windows.sampling_weights(args.synthetic_weight, args.controlled_synthetic_weight),
                                    len(training_windows), replacement=True)
               if args.synthetic_weight != 1 or args.controlled_synthetic_weight not in (None, 1) else None)
    loader = DataLoader(training_windows, batch_size=8, shuffle=sampler is None, sampler=sampler, num_workers=0)
    positives = sum(np.nansum(r["targets"], axis=1) for r in training)
    total = sum(np.isfinite(r["targets"]).sum(axis=1) for r in training)
    pos_weight = np.minimum(20.0, (total - positives) / np.maximum(positives, 1))
    pos_weight[2] = 1.0
    loss_function = nn.BCEWithLogitsLoss(pos_weight=torch.tensor(pos_weight, dtype=torch.float32, device=device)[None, :, None], reduction="none")
    if args.structure_only:
        optimizer = torch.optim.AdamW(pitch_structure.train_new_inputs_only(model), lr=1e-4 * args.learning_rate_scale, weight_decay=0)
    else:
        optimizer = torch.optim.AdamW([{"params": list(model.input_projection.parameters()) + list(model.temporal_blocks.parameters()), "lr": 1e-4 * args.learning_rate_scale},
                                      {"params": model.output_heads.parameters(), "lr": 8e-4 * args.learning_rate_scale}], weight_decay=0.01)
    history = []; best_score = -1.0; best_state = None; best_epoch = 0
    thresholds = (0.5, 0.5, 0.5)
    started = time.monotonic()
    if args.resume:
        # Keep the incumbent eligible: extra training must improve validation at its working thresholds.
        thresholds, evaluation = calibrate(predict(model, validation, device), validation)
        best_score = selection_score(evaluation)
        best_state = {key: value.detach().cpu().clone() for key, value in model.state_dict().items()}
        history.append({"epoch": 0, "loss": None, "validation": evaluation, "thresholds": thresholds})
        print(json.dumps({"epoch": 0, "validation_score": best_score, "thresholds": thresholds}), flush=True)
    for epoch in range(1, args.epochs + 1):
        model.train(); loss_sum = 0.0
        for x, y in loader:
            x, y = x.to(device), y.to(device)
            perturbation = torch.randn_like(x) * 0.025
            if args.voicing: perturbation[:, multiscale.FEATURES:] = 0
            x = x + perturbation
            valid = torch.isfinite(y)
            expected = torch.nan_to_num(y)
            optimizer.zero_grad(set_to_none=True)
            loss = (loss_function(model(x), expected) * valid
                    * boundary_loss_weights(expected, args.interior_boundary_weight)).sum() / valid.sum().clamp_min(1)
            loss.backward(); nn.utils.clip_grad_norm_(model.parameters(), 3.0); optimizer.step()
            loss_sum += float(loss.detach().cpu())
        if epoch == 1 or epoch % 5 == 0 or epoch == args.epochs:
            probabilities = predict(model, validation, device)
            candidate_thresholds, evaluation = calibrate(probabilities, validation)
            score = selection_score(evaluation)
            record = {"epoch": epoch, "loss": loss_sum / len(loader), "validation": evaluation,
                      "thresholds": candidate_thresholds}
            history.append(record)
            if args.save_epoch_checkpoints:
                directory = args.output / "epoch-checkpoints"
                directory.mkdir(exist_ok=True)
                torch.save({"schema": schema, "model_state": {key: value.detach().cpu() for key, value in model.state_dict().items()},
                            "training": record}, directory / f"epoch-{epoch:04}.pt")
            if score > best_score:
                best_score = score; best_epoch = epoch
                thresholds = candidate_thresholds
                best_state = {key: value.detach().cpu().clone() for key, value in model.state_dict().items()}
            print(json.dumps({"epoch": epoch, "loss": record["loss"], "validation_score": score, "best_epoch": best_epoch, "elapsed_s": round(time.monotonic()-started)}), flush=True)
    model.load_state_dict(best_state)
    probabilities = predict(model, validation, device)
    report = {"dataset_seconds": sum(r["seconds"] for r in records), "dataset_clips": len(records),
              "dataset_notes": sum(len(r["notes"]) for r in records), "parameters": sum(p.numel() for p in model.parameters()),
              "best_epoch": best_epoch, "epochs": args.epochs, "seed": args.seed, "device": str(device),
              "selection_policy": "validation_calibrated_mean_boundary_f1_with_resume_incumbent",
              "draft_policy": DRAFT_POLICY,
              "native_runner_sha256": digest(args.native_runner) if args.native_runner else None,
              "learning_rate_scale": args.learning_rate_scale,
              "adaptation_policy": "new_pitch_inputs_only" if args.structure_only else "full_fine_tune",
              "effective_trained_parameters": 64 * len(pitch_structure.EXTRA_FEATURES) if args.structure_only else sum(p.numel() for p in model.parameters()),
              "interior_boundary_weight": args.interior_boundary_weight,
              "correction_pairs_sha256": digest(args.correction_pairs) if args.correction_pairs else None,
              "correction_repeats": args.correction_repeats,
              "training_windows": len(training_windows),
              "context_frames": 95 if args.multiscale else 31,
              "feature_schema": schema,
              "pretrained": str(args.pretrained) if args.pretrained else None,
              "pretrained_sha256": digest(args.pretrained) if args.pretrained else None,
              "resume_sha256": digest(args.resume) if args.resume else None,
              "prepared_manifest_sha256": digest(args.prepared_manifest) if args.prepared_manifest else None,
              "split_scope": ("different_songs_user_declared" if args.distinct_songs else "explicit_session_groups" if groups or all(not r["group"].startswith("clip:") for r in records) else "whole_clip_only_unknown_song_relationships"),
              "review_policy": "frozen_review_authority_and_inherited_human_boundaries" if args.prepared_manifest else "explicit_user_declaration" if args.accept_user_reviewed else "human_reviewed_flags_only",
              "augmentation_clips": sum("augmentation" in r for r in records),
              "controlled_synthesis_clips": sum("synthesis" in r for r in records),
              "controlled_synthesis_sampling_weight": args.synthetic_weight if args.controlled_synthetic_weight is None else args.controlled_synthetic_weight,
              "synthetic_sampling_weight": args.synthetic_weight,
              "split_ids": {s: [r["id"] for r in records if r["split"] == s] for s in ("train", "validation", "test")},
              "validation_calibrated": metrics(probabilities, validation, thresholds),
              "test_frozen_thresholds": None if args.skip_test else metrics(predict(model, test, device), test, thresholds),
              "thresholds": thresholds, "history": history, "elapsed_seconds": time.monotonic()-started,
              "pitch_supervised": False, "boundary_policy": "acoustic_attack_to_musical_offset"}
    identity = hashlib.sha256("".join(r["annotation_sha256"] for r in records).encode())
    for key in sorted(best_state): identity.update(best_state[key].numpy().tobytes())
    identity.update(json.dumps(thresholds).encode())
    model_id = args.model_id + "-" + identity.hexdigest()[:12]
    export(model, thresholds, args.output / "model.json", model_id, report, args.name)
    torch.save({"schema": schema, "model_state": best_state, "training": report}, args.output / "checkpoint.pt")
    (args.output / "report.json").write_text(json.dumps(report, indent=2, allow_nan=False))
    print(json.dumps({"model_id": model_id, "parameters": report["parameters"], "best_epoch": best_epoch,
                      "thresholds": thresholds, "test": report["test_frozen_thresholds"]}), flush=True)


if __name__ == "__main__":
    main()
