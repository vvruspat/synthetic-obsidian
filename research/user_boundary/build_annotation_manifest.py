#!/usr/bin/env python3
"""Build a BoundaryNet manifest from a manually reviewed annotation project."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
from typing import Any

import soundfile as sf


EVENT_NAMES = ("phoneme", "syllable", "breath", "silence")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--annotation", type=Path, required=True)
    parser.add_argument("--audio", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--chunk-seconds", type=float, default=20.0)
    parser.add_argument("--validation-every", type=int, default=5)
    return parser.parse_args()


def clipped_intervals(
    regions: list[dict[str, Any]],
    kinds: set[str],
    chunk_start: float,
    chunk_end: float,
) -> list[list[float]]:
    output = []
    for region in regions:
        if str(region.get("kind", "")).lower() not in kinds:
            continue
        start = max(chunk_start, float(region.get("start", 0.0)))
        end = min(chunk_end, float(region.get("end", start)))
        if end - start >= 0.04:
            output.append([round(start - chunk_start, 6), round(end - chunk_start, 6)])
    return output


def event_times(
    boundaries: list[dict[str, Any]],
    kinds: set[str],
    chunk_start: float,
    chunk_end: float,
) -> list[float]:
    return [
        round(float(boundary["time"]) - chunk_start, 6)
        for boundary in boundaries
        if str(boundary.get("kind", "")).lower() in kinds
        and chunk_start <= float(boundary.get("time", -1.0)) < chunk_end
    ]


def main() -> int:
    args = parse_args()
    annotation_path = args.annotation.expanduser().resolve()
    annotation = json.loads(annotation_path.read_text(encoding="utf-8"))
    audio_path = (
        args.audio.expanduser().resolve()
        if args.audio is not None
        else annotation_path.parent / str(annotation["audio"])
    )
    output_dir = args.output_dir.expanduser().resolve()
    audio_dir = output_dir / "audio"
    audio_dir.mkdir(parents=True, exist_ok=True)

    audio_info = sf.info(str(audio_path))
    duration = min(float(annotation["duration"]), float(audio_info.duration))
    chunk_seconds = max(4.0, float(args.chunk_seconds))
    validation_every = max(2, int(args.validation_every))
    boundaries = list(annotation.get("boundaries", []))
    regions = list(annotation.get("regions", []))

    records = []
    chunk_index = 0
    chunk_start = 0.0
    while chunk_start < duration - 0.25:
        chunk_end = min(duration, chunk_start + chunk_seconds)
        start_frame = int(round(chunk_start * audio_info.samplerate))
        end_frame = int(round(chunk_end * audio_info.samplerate))
        audio, sample_rate = sf.read(
            str(audio_path),
            start=start_frame,
            frames=max(1, end_frame - start_frame),
            dtype="float32",
            always_2d=True,
        )
        chunk_name = f"manual_{chunk_index:03d}.wav"
        sf.write(audio_dir / chunk_name, audio, sample_rate, subtype="PCM_16")

        syllables = event_times(
            boundaries,
            {"syllable", "rearticulation"},
            chunk_start,
            chunk_end,
        )
        breath_intervals = clipped_intervals(
            regions,
            {"breath"},
            chunk_start,
            chunk_end,
        )
        silence_intervals = clipped_intervals(
            regions,
            {"noise", "pause"},
            chunk_start,
            chunk_end,
        )
        events = {
            "phoneme": [],
            "syllable": syllables,
            "breath": [interval[0] for interval in breath_intervals],
            "silence": [interval[0] for interval in silence_intervals],
        }
        digest = hashlib.sha256(
            f"{annotation_path}:{chunk_start:.6f}:{chunk_end:.6f}".encode()
        ).hexdigest()[:16]
        records.append(
            {
                "manifest_version": 1,
                "id": digest,
                "audio": f"audio/{chunk_name}",
                "annotation": None,
                "split": (
                    "validation"
                    if (chunk_index + 1) % validation_every == 0
                    else "train"
                ),
                "group_key": "synthetic_obsidian_manual_vocal",
                "language": "unknown",
                "singer": "user_authorized",
                "technique": "manual_annotation",
                "song": audio_path.stem,
                "group": "manual_annotation",
                "clip": f"{chunk_index:03d}",
                "sample_rate": int(sample_rate),
                "channels": int(audio.shape[1]),
                "num_frames": int(audio.shape[0]),
                "duration_seconds": round(chunk_end - chunk_start, 6),
                "events": events,
                "intervals": {
                    "breath": breath_intervals,
                    "silence": silence_intervals,
                },
                "event_counts": {name: len(events[name]) for name in EVENT_NAMES},
                "loss_heads": ["syllable", "breath", "silence"],
                "label_notes": {
                    "source": "manually reviewed Synthetic Obsidian annotation",
                    "syllable": "syllable and rearticulation boundary markers",
                    "breath": "manually reviewed breath regions",
                    "silence": "manually reviewed noise and pause regions",
                },
            }
        )
        chunk_index += 1
        chunk_start = chunk_end

    manifest_path = output_dir / "manifest.jsonl"
    manifest_path.write_text(
        "".join(json.dumps(record, ensure_ascii=False) + "\n" for record in records),
        encoding="utf-8",
    )
    summary = {
        "schema": "synthetic-obsidian.user-boundary-manifest.v1",
        "annotation": str(annotation_path),
        "audio": str(audio_path),
        "duration_seconds": duration,
        "records": len(records),
        "train_records": sum(record["split"] == "train" for record in records),
        "validation_records": sum(
            record["split"] == "validation" for record in records
        ),
        "events": {
            name: sum(record["event_counts"][name] for record in records)
            for name in EVENT_NAMES
        },
    }
    (output_dir / "summary.json").write_text(
        json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8",
    )
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
