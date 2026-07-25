#!/usr/bin/env python3
"""Train the compact backing-vocal Transformer with MLX on Apple Silicon."""

from __future__ import annotations

import argparse
import json
import math
import random
import time
from pathlib import Path
from typing import Any

import mlx.core as mx
import mlx.nn as nn
import mlx.optimizers as optim
from mlx.utils import tree_flatten
import numpy as np

from model import BackingTransformer, ModelConfig, OFFSET_MIN, continuous_features


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-root", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--epochs", type=int, default=12)
    parser.add_argument("--batch-size", type=int, default=64)
    parser.add_argument("--learning-rate", type=float, default=3e-4)
    parser.add_argument("--dimensions", type=int, default=96)
    parser.add_argument("--heads", type=int, default=4)
    parser.add_argument("--layers", type=int, default=4)
    parser.add_argument("--max-notes", type=int, default=64)
    parser.add_argument("--seed", type=int, default=42)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    random.seed(args.seed)
    np.random.seed(args.seed)
    manifest = json.loads((args.data_root / "manifest.json").read_text(encoding="utf-8"))
    styles = tuple(manifest["styles"])
    config = ModelConfig(styles, args.dimensions, args.heads, args.layers, args.dimensions * 2, args.max_notes)
    style_to_id = {style: index for index, style in enumerate(styles)}
    train_rows = load_rows(args.data_root / "train.jsonl", style_to_id, args.max_notes)
    valid_rows = load_rows(args.data_root / "valid.jsonl", style_to_id, args.max_notes)
    if not train_rows or not valid_rows:
        raise SystemExit("Both train and validation splits must contain examples")

    model = BackingTransformer(config)
    mx.eval(model.parameters())
    optimizer = optim.AdamW(learning_rate=args.learning_rate, weight_decay=0.01)

    def loss_fn(model: BackingTransformer, batch: dict[str, mx.array]) -> mx.array:
        logits = model(batch["pitch"], batch["style"], batch["chord_root"], batch["chord_quality"], batch["key_pc"], batch["key_mode"], batch["continuous"], batch["valid"])
        losses = nn.losses.cross_entropy(logits, batch["target"])
        return mx.sum(losses * batch["valid"]) / mx.maximum(mx.sum(batch["valid"]), mx.array(1.0))

    loss_and_grad = nn.value_and_grad(model, loss_fn)
    best_accuracy = -1.0
    args.output.mkdir(parents=True, exist_ok=True)
    print(json.dumps({"train_examples": len(train_rows), "valid_examples": len(valid_rows), "parameters": count_parameters(model), "config": config.as_dict()}, ensure_ascii=False), flush=True)

    for epoch in range(1, args.epochs + 1):
        started = time.monotonic()
        random.shuffle(train_rows)
        losses = []
        model.train(True)
        for start in range(0, len(train_rows), args.batch_size):
            batch = make_batch(train_rows[start : start + args.batch_size], args.max_notes)
            loss, gradients = loss_and_grad(model, batch)
            optimizer.update(model, gradients)
            mx.eval(model.parameters(), optimizer.state, loss)
            losses.append(float(loss.item()))
        metrics = evaluate(model, valid_rows, args.batch_size, args.max_notes)
        metrics.update({"epoch": epoch, "train_loss": sum(losses) / max(1, len(losses)), "seconds": round(time.monotonic() - started, 2)})
        print(json.dumps(metrics), flush=True)
        if metrics["accuracy"] > best_accuracy:
            best_accuracy = metrics["accuracy"]
            model.save_weights(str(args.output / "model.safetensors"))
            (args.output / "config.json").write_text(json.dumps(config.as_dict(), ensure_ascii=False, indent=2), encoding="utf-8")
            (args.output / "metrics.json").write_text(json.dumps(metrics, indent=2), encoding="utf-8")


def load_rows(path: Path, style_to_id: dict[str, int], max_notes: int) -> list[dict[str, Any]]:
    rows = []
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            row = json.loads(line)
            if 0 < len(row["notes"]) <= max_notes and row["style"] in style_to_id:
                row["style_id"] = style_to_id[row["style"]]
                rows.append(row)
    return rows


def make_batch(rows: list[dict[str, Any]], max_notes: int) -> dict[str, mx.array]:
    batch = len(rows)
    pitch = np.zeros((batch, max_notes), dtype=np.int32)
    chord_root = np.full((batch, max_notes), 12, dtype=np.int32)
    chord_quality = np.zeros((batch, max_notes), dtype=np.int32)
    continuous = np.zeros((batch, max_notes, 20), dtype=np.float32)
    target = np.zeros((batch, max_notes), dtype=np.int32)
    valid = np.zeros((batch, max_notes), dtype=np.bool_)
    style = np.zeros(batch, dtype=np.int32)
    key_pc = np.full(batch, 12, dtype=np.int32)
    key_mode = np.zeros(batch, dtype=np.int32)
    for row_index, row in enumerate(rows):
        style[row_index] = row["style_id"]
        key_pc[row_index] = row["key_pc"]
        key_mode[row_index] = row["key_mode"]
        for note_index, note in enumerate(row["notes"][:max_notes]):
            pitch[row_index, note_index] = note["pitch"]
            chord_root[row_index, note_index] = note["chord_root"]
            chord_quality[row_index, note_index] = note["chord_quality"]
            continuous[row_index, note_index] = continuous_features(note)
            target[row_index, note_index] = int(note["target_offset"]) - OFFSET_MIN
            valid[row_index, note_index] = True
    return {key: mx.array(value) for key, value in {"pitch": pitch, "style": style, "chord_root": chord_root, "chord_quality": chord_quality, "key_pc": key_pc, "key_mode": key_mode, "continuous": continuous, "target": target, "valid": valid}.items()}


def evaluate(model: BackingTransformer, rows: list[dict[str, Any]], batch_size: int, max_notes: int) -> dict[str, float]:
    model.train(False)
    correct = 0
    within_one = 0
    notes = 0
    total_loss = 0.0
    batches = 0
    for start in range(0, len(rows), batch_size):
        batch = make_batch(rows[start : start + batch_size], max_notes)
        logits = model(batch["pitch"], batch["style"], batch["chord_root"], batch["chord_quality"], batch["key_pc"], batch["key_mode"], batch["continuous"], batch["valid"])
        predicted = mx.argmax(logits, axis=-1)
        mask = batch["valid"]
        correct += int(mx.sum((predicted == batch["target"]) * mask).item())
        within_one += int(mx.sum((mx.abs(predicted - batch["target"]) <= 1) * mask).item())
        notes += int(mx.sum(mask).item())
        loss = nn.losses.cross_entropy(logits, batch["target"])
        total_loss += float((mx.sum(loss * mask) / mx.maximum(mx.sum(mask), mx.array(1.0))).item())
        batches += 1
    return {"valid_loss": total_loss / max(1, batches), "accuracy": correct / max(1, notes), "within_one_semitone": within_one / max(1, notes), "valid_notes": notes}


def count_parameters(model: nn.Module) -> int:
    return sum(int(math.prod(parameter.shape)) for _, parameter in tree_flatten(model.parameters()))


if __name__ == "__main__":
    main()
