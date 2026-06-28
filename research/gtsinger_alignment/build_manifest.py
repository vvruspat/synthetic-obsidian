#!/usr/bin/env python3
from __future__ import annotations

import argparse
import hashlib
import json
import sys
import wave
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator

try:
    from .labels import derive_labels
except ImportError:
    from labels import derive_labels


MANIFEST_VERSION = 1


@dataclass(frozen=True)
class AudioInfo:
    sample_rate: int
    channels: int
    frames: int
    duration_seconds: float


def read_wav_info(path: Path) -> AudioInfo:
    with wave.open(str(path), "rb") as wav_file:
        sample_rate = wav_file.getframerate()
        channels = wav_file.getnchannels()
        frames = wav_file.getnframes()
    if sample_rate <= 0 or channels <= 0 or frames <= 0:
        raise ValueError("WAV has invalid stream metadata")
    return AudioInfo(
        sample_rate=sample_rate,
        channels=channels,
        frames=frames,
        duration_seconds=frames / sample_rate,
    )


def stable_split(group_key: str) -> str:
    bucket = int(hashlib.sha1(group_key.encode("utf-8")).hexdigest()[:8], 16) % 100
    if bucket < 80:
        return "train"
    if bucket < 90:
        return "validation"
    return "test"


def stable_sample_key(path: Path, seed: int) -> str:
    return hashlib.sha1(f"{seed}:{path.as_posix()}".encode("utf-8")).hexdigest()


def iter_audio_files(
    dataset_root: Path,
    limit: int | None,
    seed: int,
    languages: set[str] | None,
    include_paired_speech: bool,
) -> Iterator[Path]:
    paths = dataset_root.rglob("*.wav")
    if not include_paired_speech:
        paths = (path for path in paths if path.parent.name != "Paired_Speech_Group")
    if languages:
        paths = (
            path
            for path in paths
            if path.relative_to(dataset_root).parts[0] in languages
        )
    if limit is None:
        yield from sorted(paths)
        return

    selected = sorted(paths, key=lambda path: stable_sample_key(path, seed))[:limit]
    yield from sorted(selected)


def parse_metadata(dataset_root: Path, audio_path: Path) -> dict[str, str]:
    parts = audio_path.relative_to(dataset_root).parts
    if len(parts) != 6:
        raise ValueError(
            "expected Language/Singer/Technique/Song/Group/clip.wav layout, "
            f"found {len(parts)} path components"
        )
    language, singer, technique, song, group, filename = parts
    return {
        "language": language,
        "singer": singer,
        "technique": technique,
        "song": song,
        "group": group,
        "clip": Path(filename).stem,
    }


def build_record(dataset_root: Path, audio_path: Path) -> dict[str, object]:
    annotation_path = audio_path.with_suffix(".json")
    if not annotation_path.is_file():
        raise ValueError("missing JSON annotation")

    metadata = parse_metadata(dataset_root, audio_path)
    audio_info = read_wav_info(audio_path)
    with annotation_path.open("r", encoding="utf-8") as annotation_file:
        annotation = json.load(annotation_file)
    labels = derive_labels(
        annotation,
        audio_info.duration_seconds,
        language=metadata["language"],
    )

    relative_audio = audio_path.relative_to(dataset_root).as_posix()
    relative_annotation = annotation_path.relative_to(dataset_root).as_posix()
    group_key = "/".join(
        (metadata["language"], metadata["singer"], metadata["song"])
    )
    record_id = hashlib.sha1(relative_audio.encode("utf-8")).hexdigest()[:16]
    return {
        "manifest_version": MANIFEST_VERSION,
        "id": record_id,
        "audio": relative_audio,
        "annotation": relative_annotation,
        "split": stable_split(group_key),
        "group_key": group_key,
        **metadata,
        "sample_rate": audio_info.sample_rate,
        "channels": audio_info.channels,
        "num_frames": audio_info.frames,
        "duration_seconds": round(audio_info.duration_seconds, 6),
        "events": labels.events,
        "intervals": labels.intervals,
        "event_counts": labels.counts,
        "label_notes": {
            "phoneme": "onset of every non-special phone",
            "syllable": "proxy: onset of every detected vowel nucleus",
            "breath": "onset of each <AP> interval",
            "silence": "onset of each <SP> interval",
        },
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build a deterministic JSONL manifest from GTSinger WAV/JSON pairs."
    )
    parser.add_argument("--dataset-root", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--limit", type=int)
    parser.add_argument(
        "--language",
        action="append",
        help="include only this language directory; may be repeated",
    )
    parser.add_argument("--seed", type=int, default=20260613)
    parser.add_argument(
        "--include-paired-speech",
        action="store_true",
        help="include Paired_Speech_Group clips; singing clips are used by default",
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="validate and summarize records without writing a manifest",
    )
    parser.add_argument(
        "--strict",
        action="store_true",
        help="fail on the first incomplete or malformed record",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    dataset_root = args.dataset_root.expanduser().resolve()
    if not dataset_root.is_dir():
        print(f"error: dataset root does not exist: {dataset_root}", file=sys.stderr)
        return 2
    if args.limit is not None and args.limit <= 0:
        print("error: --limit must be positive", file=sys.stderr)
        return 2
    if not args.dry_run and args.output is None:
        print("error: --output is required unless --dry-run is used", file=sys.stderr)
        return 2

    records: list[dict[str, object]] = []
    skipped: list[dict[str, str]] = []
    languages = set(args.language) if args.language else None
    for audio_path in iter_audio_files(
        dataset_root, args.limit, args.seed, languages, args.include_paired_speech
    ):
        try:
            records.append(build_record(dataset_root, audio_path))
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            if args.strict:
                raise
            skipped.append(
                {
                    "audio": audio_path.relative_to(dataset_root).as_posix(),
                    "reason": str(exc),
                }
            )

    if not records:
        print("error: no valid WAV/JSON records found", file=sys.stderr)
        return 1

    if not args.dry_run:
        output = args.output.expanduser().resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("w", encoding="utf-8") as manifest_file:
            for record in records:
                manifest_file.write(
                    json.dumps(record, ensure_ascii=False, sort_keys=True) + "\n"
                )
        if skipped:
            skipped_path = output.with_suffix(output.suffix + ".skipped.json")
            skipped_path.write_text(
                json.dumps(skipped, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )

    split_counts = Counter(str(record["split"]) for record in records)
    language_counts = Counter(str(record["language"]) for record in records)
    event_counts = Counter()
    total_duration = 0.0
    for record in records:
        total_duration += float(record["duration_seconds"])
        event_counts.update(record["event_counts"])

    summary = {
        "dataset_root": str(dataset_root),
        "output": None if args.dry_run else str(args.output.expanduser().resolve()),
        "dry_run": args.dry_run,
        "language_filter": sorted(languages) if languages else None,
        "include_paired_speech": args.include_paired_speech,
        "records": len(records),
        "skipped": len(skipped),
        "duration_hours": round(total_duration / 3600.0, 3),
        "splits": dict(sorted(split_counts.items())),
        "languages": dict(sorted(language_counts.items())),
        "events": dict(sorted(event_counts.items())),
    }
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    if skipped:
        print("First skipped records:", file=sys.stderr)
        for item in skipped[:5]:
            print(f"  {item['audio']}: {item['reason']}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
