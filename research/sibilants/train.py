from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import random
import time

import numpy as np
import torch
from torch import nn
from torch.utils.data import Dataset, DataLoader, WeightedRandomSampler

from .pipeline import CONTEXT_FRAMES, DILATIONS, FEATURES, HOP, MELS, N_FFT, SAMPLE_RATE, SCHEMA, SibilantNet, frame_metrics


class Windows(Dataset):
    def __init__(self, records, mean, std, window=512):
        self.examples = []
        self.index = []
        self.weights = []
        self.window = window
        for record in records:
            x = ((np.load(record["features"]) - mean[:, None]) / std[:, None]).astype(np.float32)
            y = np.load(record["targets"])
            index = len(self.examples)
            self.examples.append((x, y))
            starts = sorted(set(range(0, max(1, x.shape[1] - window + 1), window // 2)) | {max(0, x.shape[1] - window)})
            for start in starts:
                target = y[start + CONTEXT_FRAMES:start + window - CONTEXT_FRAMES]
                if np.isfinite(target).sum() > 50:
                    self.index.append((index, start))
                    self.weights.append(1.0 + 2.0 * float(np.sum(target == 1) > 10))

    def __len__(self):
        return len(self.index)

    def __getitem__(self, index):
        example, start = self.index[index]
        x, y = self.examples[example]
        value, target = x[:, start:start + self.window].copy(), y[start:start + self.window].copy()
        if value.shape[1] < self.window:
            count = self.window - value.shape[1]
            value = np.pad(value, ((0, 0), (0, count)))
            target = np.pad(target, (0, count), constant_values=np.nan)
        target[:CONTEXT_FRAMES] = np.nan
        target[-CONTEXT_FRAMES:] = np.nan
        return torch.from_numpy(value), torch.from_numpy(target)


def probabilities(model, feature_values, mean, std, device):
    output = np.empty(feature_values.shape[1], np.float32)
    model.eval()
    with torch.inference_mode():
        for start in range(0, len(output), 4096):
            end = min(len(output), start + 4096)
            lo, hi = max(0, start - CONTEXT_FRAMES), min(len(output), end + CONTEXT_FRAMES)
            x = ((feature_values[:, lo:hi] - mean[:, None]) / std[:, None]).astype(np.float32)
            prediction = torch.sigmoid(model(torch.from_numpy(x[None]).to(device)))[0].cpu().numpy()
            output[start:end] = prediction[start - lo:end - lo]
    return output


def evaluate(model, records, mean, std, device):
    predictions, targets = [], []
    for record in records:
        predictions.append(probabilities(model, np.load(record["features"]), mean, std, device))
        targets.append(np.load(record["targets"]))
    return np.concatenate(predictions), np.concatenate(targets)


def thresholds(predictions, targets):
    rows = [(float(t), frame_metrics(predictions, targets, float(t))) for t in np.arange(0.05, 0.996, 0.01)]
    best = max(rows, key=lambda row: row[1]["f1"])
    strict_rows = [row for row in rows if row[0] >= max(0.5, best[0]) - 1e-9]
    precise = [row for row in strict_rows if row[1]["precision"] >= 0.995 and row[1]["recall"] >= 0.1]
    conservative = max(precise, key=lambda row: row[1]["recall"]) if precise else max(strict_rows, key=lambda row: 1.25 * row[1]["precision"] * row[1]["recall"] / max(1e-12, 0.25 * row[1]["precision"] + row[1]["recall"]))
    return best, conservative


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--epochs", type=int, default=40)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    seed = 1729
    random.seed(seed); np.random.seed(seed); torch.manual_seed(seed); torch.set_num_threads(4)
    device = torch.device("mps" if torch.backends.mps.is_available() else "cpu")
    manifest = json.loads(args.manifest.read_text())
    roles = {role: [r for r in manifest["records"] if r["split"] == role] for role in ("train", "validation", "test")}
    sums, squares, count = np.zeros(FEATURES), np.zeros(FEATURES), 0
    for record in roles["train"]:
        x = np.load(record["features"]).astype(np.float64)
        sums += x.sum(axis=1); squares += np.square(x).sum(axis=1); count += x.shape[1]
    mean = (sums / count).astype(np.float32)
    std = np.maximum(np.sqrt(np.maximum(0, squares / count - mean.astype(np.float64) ** 2)), 0.001).astype(np.float32)
    dataset = Windows(roles["train"], mean, std)
    loader = DataLoader(dataset, batch_size=16, sampler=WeightedRandomSampler(dataset.weights, len(dataset), replacement=True), num_workers=0)
    model = SibilantNet().to(device)
    optimizer = torch.optim.AdamW(model.parameters(), lr=0.0005, weight_decay=0.0001)
    positive_weight = torch.tensor(2.0, device=device)
    best_score, best_epoch, history, best_state = -1.0, 0, [], None
    began = time.time()
    print(json.dumps({"device": str(device), "parameters": sum(p.numel() for p in model.parameters()), "training_windows": len(dataset), "steps_per_epoch": len(loader)}), flush=True)
    for epoch in range(1, args.epochs + 1):
        model.train()
        total_loss = 0.0
        epoch_began = time.time()
        for value, target in loader:
            value, target = value.to(device), target.to(device)
            optimizer.zero_grad(set_to_none=True)
            valid = torch.isfinite(target)
            expected = torch.nan_to_num(target)
            losses = nn.functional.binary_cross_entropy_with_logits(model(value), expected, pos_weight=positive_weight, reduction="none")
            loss = (losses * valid).sum() / valid.sum().clamp(min=1)
            loss.backward(); nn.utils.clip_grad_norm_(model.parameters(), 5.0); optimizer.step()
            total_loss += float(loss.detach().cpu())
        predictions, expected = evaluate(model, roles["validation"], mean, std, device)
        balanced, conservative = thresholds(predictions, expected)
        row = {"epoch": epoch, "loss": total_loss / len(loader), "validation_threshold": balanced[0],
               "validation": balanced[1], "conservative_threshold": conservative[0],
               "conservative_validation": conservative[1], "seconds": round(time.time() - epoch_began, 2)}
        history.append(row)
        if balanced[1]["f1"] > best_score + 0.0001:
            best_score, best_epoch = balanced[1]["f1"], epoch
            best_state = copy.deepcopy({k: v.detach().cpu() for k, v in model.state_dict().items()})
            torch.save({"schema": SCHEMA, "state": best_state, "mean": mean, "std": std,
                        "epoch": best_epoch, "channels": 48}, args.output / "checkpoint.pt")
        (args.output / "history.json").write_text(json.dumps(history, indent=2))
        print(json.dumps(row), flush=True)
        if epoch - best_epoch >= 8:
            break
    model.load_state_dict(best_state)
    predictions, expected = evaluate(model, roles["validation"], mean, std, device)
    balanced, conservative = thresholds(predictions, expected)
    test_predictions, test_expected = evaluate(model, roles["test"], mean, std, device)
    np.savez_compressed(args.output / "heldout-predictions.npz", validation_probabilities=predictions,
                        validation_targets=expected, test_probabilities=test_predictions, test_targets=test_expected)
    state = {k: v.numpy().tolist() for k, v in best_state.items()}
    identity = hashlib.sha256(json.dumps(state, sort_keys=True, separators=(",", ":")).encode()).hexdigest()[:12]
    contract = {"schema": SCHEMA, "sample_rate": SAMPLE_RATE, "hop": HOP, "n_fft": N_FFT, "mels": MELS,
                "model_id": "sasha-sibilants-v1-" + identity, "name": "Sasha sibilants v1 (experimental)",
                "feature_count": FEATURES, "channels": 48, "dilations": list(DILATIONS),
                "output": "sibilant_probability", "input_mean": mean.tolist(), "input_std": std.tolist(),
                "balanced_threshold": balanced[0], "conservative_threshold": conservative[0],
                "state": state, "source_manifest": str(args.manifest.resolve()), "epoch": best_epoch,
                "label_rule": manifest.get("label_rule"), "source_pair": manifest["input_pair"]}
    (args.output / "model.json").write_text(json.dumps(contract, separators=(",", ":")))
    model_cpu = SibilantNet(); model_cpu.load_state_dict(best_state); model_cpu.eval()
    fixture = np.load(roles["validation"][0]["features"])[:, :1000]
    cpu = probabilities(model_cpu, fixture, mean, std, torch.device("cpu"))
    accelerated = probabilities(model, fixture, mean, std, device)
    parity = float(np.max(np.abs(cpu - accelerated)))
    if parity > 0.0001:
        raise ValueError(f"CPU/accelerator parity failed: {parity}")
    report = {"schema": SCHEMA, "device": str(device), "parameters": sum(p.numel() for p in model.parameters()),
              "best_epoch": best_epoch, "epochs_run": len(history), "elapsed_seconds": round(time.time() - began, 2),
              "balanced_threshold": balanced[0], "validation": balanced[1],
              "conservative_threshold": conservative[0], "conservative_validation": conservative[1],
              "test": frame_metrics(test_predictions, test_expected, balanced[0]),
              "conservative_test": frame_metrics(test_predictions, test_expected, conservative[0]),
              "conservative_selection": "Validation-only: threshold >= max(0.5, balanced threshold), target precision >= 99.5%, maximize recall.",
              "cpu_mps_max_probability_delta": parity, "scope": manifest["scope"],
              "note": "Frame metrics against the user-isolated stem; they do not prove correct note attachment or generalization to unseen singers."}
    (args.output / "report.json").write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2), flush=True)


if __name__ == "__main__":
    main()
