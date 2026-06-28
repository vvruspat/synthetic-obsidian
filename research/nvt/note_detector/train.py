from __future__ import annotations

import argparse
import json
from pathlib import Path

import torch
from torch.utils.data import DataLoader

from nvt.note_detector.dataset import NoteDetectorDataset, collate_note_detector_batch
from nvt.note_detector.model import NoteDetectorLoss, NoteDetectorModel


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Train baseline vocal note detector.")
    parser.add_argument(
        "--manifest",
        type=Path,
        default=Path("data/note_detector/local_segments/manifest.jsonl"),
        help="Path to manifest.jsonl produced by import_midi_vox_dataset.py",
    )
    parser.add_argument("--output-dir", type=Path, default=Path("research/checkpoints_note_detector"))
    parser.add_argument(
        "--init-checkpoint",
        type=Path,
        default=None,
        help="Optional checkpoint whose model weights will be used to initialize training.",
    )
    parser.add_argument("--sample-rate", type=int, default=24000)
    parser.add_argument("--n-mels", type=int, default=80)
    parser.add_argument("--hop-length", type=int, default=256)
    parser.add_argument("--n-fft", type=int, default=1024)
    parser.add_argument("--midi-min", type=int, default=36)
    parser.add_argument("--midi-max", type=int, default=84)
    parser.add_argument("--batch-size", type=int, default=8)
    parser.add_argument("--epochs", type=int, default=8)
    parser.add_argument("--lr", type=float, default=2e-4)
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    parser.add_argument("--log-every", type=int, default=20)
    parser.add_argument("--num-workers", type=int, default=0)
    parser.add_argument("--limit-train", type=int, default=0)
    parser.add_argument("--limit-val", type=int, default=0)
    return parser.parse_args()


def load_init_weights(model: NoteDetectorModel, checkpoint_path: Path, device: torch.device) -> None:
    state = torch.load(str(checkpoint_path), map_location=device, weights_only=False)
    model.load_state_dict(state["model"], strict=True)
    epoch = state.get("epoch")
    print(f"Loaded init checkpoint: {checkpoint_path} (epoch={epoch})")


def maybe_limit(dataset: NoteDetectorDataset, limit: int) -> NoteDetectorDataset:
    if limit <= 0 or limit >= len(dataset):
        return dataset
    dataset.entries = dataset.entries[:limit]
    return dataset


def run_epoch(
    model: NoteDetectorModel,
    loader: DataLoader,
    criterion: NoteDetectorLoss,
    optimizer: torch.optim.Optimizer | None,
    device: torch.device,
    log_every: int,
) -> dict[str, float]:
    is_train = optimizer is not None
    model.train(is_train)

    totals = {"total": 0.0, "voiced": 0.0, "onset": 0.0, "pitch": 0.0}

    for step, batch in enumerate(loader, start=1):
        tensor_batch = {
            key: value.to(device) if isinstance(value, torch.Tensor) else value
            for key, value in batch.items()
        }

        with torch.set_grad_enabled(is_train):
            predictions = model(tensor_batch["log_mel"])  # type: ignore[arg-type]
            losses = criterion(predictions, tensor_batch)

        if is_train and optimizer is not None:
            optimizer.zero_grad(set_to_none=True)
            losses["total"].backward()
            torch.nn.utils.clip_grad_norm_(model.parameters(), max_norm=1.0)
            optimizer.step()

        for key in totals:
            totals[key] += float(losses[key].detach().cpu())

        if is_train and step % log_every == 0:
            print(
                f"  step {step:04d} "
                f"total={losses['total'].item():.4f} "
                f"voiced={losses['voiced'].item():.4f} "
                f"onset={losses['onset'].item():.4f} "
                f"pitch={losses['pitch'].item():.4f}"
            )

    denom = max(1, len(loader))
    return {key: value / denom for key, value in totals.items()}


def main() -> int:
    args = parse_args()
    device = torch.device(args.device)

    train_dataset = maybe_limit(
        NoteDetectorDataset(
            manifest_path=args.manifest,
            split="train",
            sample_rate=args.sample_rate,
            n_fft=args.n_fft,
            hop_length=args.hop_length,
            n_mels=args.n_mels,
            midi_min=args.midi_min,
            midi_max=args.midi_max,
        ),
        args.limit_train,
    )
    val_dataset = maybe_limit(
        NoteDetectorDataset(
            manifest_path=args.manifest,
            split="val",
            sample_rate=args.sample_rate,
            n_fft=args.n_fft,
            hop_length=args.hop_length,
            n_mels=args.n_mels,
            midi_min=args.midi_min,
            midi_max=args.midi_max,
        ),
        args.limit_val,
    )

    train_loader = DataLoader(
        train_dataset,
        batch_size=args.batch_size,
        shuffle=True,
        num_workers=args.num_workers,
        collate_fn=collate_note_detector_batch,
    )
    val_loader = DataLoader(
        val_dataset,
        batch_size=args.batch_size,
        shuffle=False,
        num_workers=args.num_workers,
        collate_fn=collate_note_detector_batch,
    )

    model = NoteDetectorModel(
        n_mels=args.n_mels,
        n_pitch_bins=args.midi_max - args.midi_min + 1,
    ).to(device)
    if args.init_checkpoint is not None:
        load_init_weights(model, args.init_checkpoint.resolve(), device)
    criterion = NoteDetectorLoss()
    optimizer = torch.optim.AdamW(model.parameters(), lr=args.lr, betas=(0.9, 0.98), weight_decay=1e-4)

    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    history: list[dict[str, float | int]] = []
    best_val = float("inf")

    for epoch in range(1, args.epochs + 1):
        print(f"\nEpoch {epoch}/{args.epochs}")
        train_metrics = run_epoch(model, train_loader, criterion, optimizer, device, args.log_every)
        val_metrics = run_epoch(model, val_loader, criterion, None, device, args.log_every)

        row: dict[str, float | int] = {"epoch": epoch}
        for prefix, metrics in (("train", train_metrics), ("val", val_metrics)):
            for key, value in metrics.items():
                row[f"{prefix}_{key}"] = value
        history.append(row)

        print(
            "  "
            f"train total={train_metrics['total']:.4f} "
            f"voiced={train_metrics['voiced']:.4f} "
            f"onset={train_metrics['onset']:.4f} "
            f"pitch={train_metrics['pitch']:.4f}"
        )
        print(
            "  "
            f"val   total={val_metrics['total']:.4f} "
            f"voiced={val_metrics['voiced']:.4f} "
            f"onset={val_metrics['onset']:.4f} "
            f"pitch={val_metrics['pitch']:.4f}"
        )

        checkpoint = {
            "epoch": epoch,
            "model": model.state_dict(),
            "optimizer": optimizer.state_dict(),
            "args": vars(args),
            "history": history,
        }
        torch.save(checkpoint, output_dir / f"epoch_{epoch:04d}.pt")

        if val_metrics["total"] < best_val:
            best_val = val_metrics["total"]
            torch.save(checkpoint, output_dir / "best.pt")

        (output_dir / "history.json").write_text(json.dumps(history, indent=2) + "\n", encoding="utf-8")

    print(f"\nDone. Best val total={best_val:.4f}")
    print(f"Checkpoints: {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
