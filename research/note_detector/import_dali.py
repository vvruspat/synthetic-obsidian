#!/usr/bin/env python3
"""Import DALI annotations into Synthetic Obsidian note-detector labels.

DALI distributes song annotations as gzipped Python pickles. The public GitHub
repository only includes the ground-truth timing correction file; the per-song
annotation files must be downloaded separately as DALI_data.
"""

from __future__ import annotations

import argparse
import gzip
import json
import math
import pickle
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


class _PickledDaliAnnotations:
    """Minimal stand-in for DALI.Annotations used only while unpickling."""

    def __init__(self, *_args: Any, **_kwargs: Any) -> None:
        self.info: dict[str, Any] = {}
        self.annotations: dict[str, Any] = {}


class _DaliUnpickler(pickle.Unpickler):
    def find_class(self, module: str, name: str) -> Any:
        if module in {"DALI.Annotations", "Annotations"} and name == "Annotations":
            return _PickledDaliAnnotations
        return super().find_class(module, name)


@dataclass(frozen=True)
class DaliNote:
    start: float
    end: float
    freq_hz: float | None
    midi: float | None
    text: str
    index: int | None


@dataclass(frozen=True)
class DaliSegment:
    start: float
    end: float
    text: str
    level: str
    index: int | None


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Convert DALI .gz annotation files to normalized JSON/JSONL labels."
    )
    parser.add_argument(
        "--dali-root",
        type=Path,
        required=True,
        help="Directory containing per-song DALI .gz files, or one .gz annotation file.",
    )
    parser.add_argument(
        "--ground-truth",
        type=Path,
        default=None,
        help="Optional gt_v*.gz timing correction file from DALI/versions.",
    )
    parser.add_argument(
        "--dali-code-root",
        type=Path,
        default=None,
        help="Optional path to the cloned DALI/code folder for unpickling DALI objects.",
    )
    parser.add_argument(
        "--audio-root",
        type=Path,
        default=None,
        help="Optional directory containing local audio files named by DALI id.",
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("data/note_detector/dali"),
        help="Destination directory for labels/ and manifest.jsonl.",
    )
    parser.add_argument(
        "--segment-level",
        choices=("lines", "words", "paragraphs"),
        default="lines",
        help="Which DALI text level to use as phrase chunks.",
    )
    parser.add_argument(
        "--min-seconds",
        type=float,
        default=0.15,
        help="Drop notes shorter than this duration.",
    )
    parser.add_argument(
        "--max-segment-seconds",
        type=float,
        default=12.0,
        help="Split/skip manifest chunks longer than this by falling back to note windows.",
    )
    parser.add_argument(
        "--require-audio",
        action="store_true",
        help="Skip entries whose local audio file cannot be found.",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=0,
        help="Import at most this many annotation entries. 0 means no limit.",
    )
    return parser.parse_args()


def read_gzip_pickle(path: Path) -> Any:
    with gzip.open(path, "rb") as handle:
        return _DaliUnpickler(handle).load()


def configure_dali_code_path(path: Path | None) -> None:
    if path is None:
        return
    sys.path.insert(0, str(path.resolve()))


def load_ground_truth(path: Path | None) -> dict[str, dict[str, float]]:
    if path is None:
        return {}
    payload = read_gzip_pickle(path)
    if not isinstance(payload, dict):
        raise ValueError(f"Ground-truth file is not a dict: {path}")
    return payload


def iter_annotation_files(path: Path) -> Iterable[Path]:
    if path.is_file():
        yield path
        return

    for candidate in sorted(path.rglob("*.gz")):
        # DALI_DATA_INFO and gt_v*.gz are metadata, not song annotation objects.
        name = candidate.name.lower()
        if "dali_data_info" in name or name.startswith("gt_"):
            continue
        yield candidate


def entry_to_dict(entry: Any) -> dict[str, Any]:
    if isinstance(entry, dict):
        return entry
    if hasattr(entry, "info") and hasattr(entry, "annotations"):
        return {"info": entry.info, "annotations": entry.annotations}
    raise ValueError(f"Unsupported DALI entry type: {type(entry)!r}")


def get_entry_id(entry: dict[str, Any], source_path: Path) -> str:
    info = entry.get("info", {})
    dali_id = info.get("id")
    if isinstance(dali_id, str) and dali_id and dali_id != "None":
        return dali_id
    return source_path.stem


def get_horizontal_annotations(entry: dict[str, Any]) -> dict[str, list[dict[str, Any]]]:
    annotations = entry.get("annotations", {})
    annot = annotations.get("annot", {})
    if annotations.get("type") == "horizontal" and isinstance(annot, dict):
        return {
            key: value
            for key, value in annot.items()
            if isinstance(value, list)
        }

    # Most downloadable DALI files are horizontal. If a vertical entry appears,
    # fail loudly so we do not silently train on empty labels.
    raise ValueError("Only horizontal DALI annotations are supported by this importer")


def note_frequency(note: dict[str, Any]) -> float | None:
    values = note.get("freq")
    if not isinstance(values, list):
        values = [values]

    freqs = []
    for value in values:
        try:
            freq = float(value)
        except (TypeError, ValueError):
            continue
        if math.isfinite(freq) and freq > 0.0:
            freqs.append(freq)

    if not freqs:
        return None
    return sum(freqs) / len(freqs)


def frequency_to_midi(freq_hz: float | None) -> float | None:
    if freq_hz is None or freq_hz <= 0.0:
        return None
    return 69.0 + 12.0 * math.log2(freq_hz / 440.0)


def parse_time(item: dict[str, Any]) -> tuple[float, float] | None:
    values = item.get("time")
    if not isinstance(values, list) or len(values) < 2:
        return None
    try:
        start = float(values[0])
        end = float(values[1])
    except (TypeError, ValueError):
        return None
    if not math.isfinite(start) or not math.isfinite(end) or end <= start:
        return None
    return start, end


def parse_notes(items: list[dict[str, Any]], min_seconds: float) -> list[DaliNote]:
    notes: list[DaliNote] = []
    for item in items:
        note_time = parse_time(item)
        if note_time is None:
            continue
        start, end = note_time
        if end - start < min_seconds:
            continue
        freq_hz = note_frequency(item)
        notes.append(
            DaliNote(
                start=start,
                end=end,
                freq_hz=freq_hz,
                midi=frequency_to_midi(freq_hz),
                text=str(item.get("text", "")),
                index=item.get("index") if isinstance(item.get("index"), int) else None,
            )
        )
    return notes


def parse_segments(items: list[dict[str, Any]], level: str) -> list[DaliSegment]:
    segments: list[DaliSegment] = []
    for item in items:
        segment_time = parse_time(item)
        if segment_time is None:
            continue
        start, end = segment_time
        segments.append(
            DaliSegment(
                start=start,
                end=end,
                text=str(item.get("text", "")),
                level=level,
                index=item.get("index") if isinstance(item.get("index"), int) else None,
            )
        )
    return segments


def find_audio_file(audio_root: Path | None, dali_id: str) -> Path | None:
    if audio_root is None:
        return None
    for suffix in (".wav", ".flac", ".mp3", ".m4a", ".ogg"):
        candidate = audio_root / f"{dali_id}{suffix}"
        if candidate.exists():
            return candidate
    return None


def notes_in_range(notes: list[DaliNote], start: float, end: float) -> list[DaliNote]:
    return [
        note
        for note in notes
        if note.end > start and note.start < end
    ]


def build_manifest_chunks(
    dali_id: str,
    label_path: Path,
    audio_path: Path | None,
    notes: list[DaliNote],
    segments: list[DaliSegment],
    max_segment_seconds: float,
) -> list[dict[str, Any]]:
    chunks: list[dict[str, Any]] = []

    for segment in segments:
        duration = segment.end - segment.start
        segment_notes = notes_in_range(notes, segment.start, segment.end)
        if not segment_notes:
            continue
        if duration <= max_segment_seconds:
            chunks.append(
                {
                    "dali_id": dali_id,
                    "audio": str(audio_path) if audio_path else None,
                    "label": str(label_path),
                    "start": segment.start,
                    "end": segment.end,
                    "text": segment.text,
                    "segment_level": segment.level,
                    "note_count": len(segment_notes),
                    "audio_missing": audio_path is None,
                }
            )
            continue

        chunks.extend(build_note_windows(dali_id, label_path, audio_path, segment_notes, max_segment_seconds))

    if chunks:
        return chunks

    return build_note_windows(dali_id, label_path, audio_path, notes, max_segment_seconds)


def build_note_windows(
    dali_id: str,
    label_path: Path,
    audio_path: Path | None,
    notes: list[DaliNote],
    max_segment_seconds: float,
) -> list[dict[str, Any]]:
    chunks: list[dict[str, Any]] = []
    window_notes: list[DaliNote] = []
    window_start: float | None = None

    for note in notes:
        if window_start is None:
            window_start = note.start
        if window_notes and note.end - window_start > max_segment_seconds:
            chunks.append(note_window_manifest(dali_id, label_path, audio_path, window_notes))
            window_notes = []
            window_start = note.start
        window_notes.append(note)

    if window_notes:
        chunks.append(note_window_manifest(dali_id, label_path, audio_path, window_notes))

    return chunks


def note_window_manifest(
    dali_id: str,
    label_path: Path,
    audio_path: Path | None,
    notes: list[DaliNote],
) -> dict[str, Any]:
    return {
        "dali_id": dali_id,
        "audio": str(audio_path) if audio_path else None,
        "label": str(label_path),
        "start": notes[0].start,
        "end": notes[-1].end,
        "text": "".join(note.text for note in notes),
        "segment_level": "note_window",
        "note_count": len(notes),
        "audio_missing": audio_path is None,
    }


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def import_entry(
    source_path: Path,
    output_dir: Path,
    ground_truth: dict[str, dict[str, float]],
    audio_root: Path | None,
    segment_level: str,
    min_seconds: float,
    max_segment_seconds: float,
    require_audio: bool,
) -> tuple[list[dict[str, Any]], str | None]:
    raw_entry = read_gzip_pickle(source_path)
    if looks_like_ground_truth(raw_entry):
        return [], f"{source_path} looks like a DALI ground-truth timing file; pass it via --ground-truth"

    entry = entry_to_dict(raw_entry)
    dali_id = get_entry_id(entry, source_path)
    audio_path = find_audio_file(audio_root, dali_id)
    if require_audio and audio_path is None:
        return [], f"missing audio for {dali_id}"

    horizontal = get_horizontal_annotations(entry)
    notes = parse_notes(horizontal.get("notes", []), min_seconds)
    segments = parse_segments(horizontal.get(segment_level, []), segment_level)
    if not notes:
        return [], f"no usable notes in {dali_id}"

    info = entry.get("info", {})
    label_path = output_dir / "labels" / f"{dali_id}.json"
    label_payload = {
        "schema": "synthetic-obsidian.note-detector.v1",
        "source": "DALI",
        "dali_id": dali_id,
        "source_file": str(source_path),
        "audio": str(audio_path) if audio_path else None,
        "audio_missing": audio_path is None,
        "title": info.get("title"),
        "artist": info.get("artist"),
        "license": "CC BY-NC-SA 4.0",
        "ground_truth_timing": ground_truth.get(dali_id),
        "notes": [note.__dict__ for note in notes],
        "segments": [segment.__dict__ for segment in segments],
    }
    write_json(label_path, label_payload)

    chunks = build_manifest_chunks(
        dali_id=dali_id,
        label_path=label_path,
        audio_path=audio_path,
        notes=notes,
        segments=segments,
        max_segment_seconds=max_segment_seconds,
    )
    return chunks, None


def looks_like_ground_truth(payload: Any) -> bool:
    if not isinstance(payload, dict) or not payload:
        return False
    sample = next(iter(payload.values()))
    return (
        isinstance(sample, dict)
        and set(sample.keys()).issuperset({"offset", "fr"})
        and "annotations" not in payload
    )


def main() -> int:
    args = parse_args()
    configure_dali_code_path(args.dali_code_root)
    ground_truth = load_ground_truth(args.ground_truth)
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    manifest: list[dict[str, Any]] = []
    warnings: list[str] = []
    imported = 0

    for source_path in iter_annotation_files(args.dali_root.resolve()):
        if args.limit and imported >= args.limit:
            break
        try:
            chunks, warning = import_entry(
                source_path=source_path,
                output_dir=output_dir,
                ground_truth=ground_truth,
                audio_root=args.audio_root.resolve() if args.audio_root else None,
                segment_level=args.segment_level,
                min_seconds=args.min_seconds,
                max_segment_seconds=args.max_segment_seconds,
                require_audio=args.require_audio,
            )
        except Exception as exc:
            warnings.append(f"{source_path}: {exc}")
            continue

        if warning:
            warnings.append(warning)
        if chunks:
            manifest.extend(chunks)
            imported += 1

    manifest_path = output_dir / "manifest.jsonl"
    with manifest_path.open("w", encoding="utf-8") as handle:
        for item in manifest:
            handle.write(json.dumps(item, ensure_ascii=False) + "\n")

    if warnings:
        warning_path = output_dir / "warnings.txt"
        warning_path.write_text("\n".join(warnings) + "\n", encoding="utf-8")

    print(f"Imported entries: {imported}")
    print(f"Manifest chunks: {len(manifest)}")
    print(f"Output: {output_dir}")
    if warnings:
        print(f"Warnings: {len(warnings)} ({output_dir / 'warnings.txt'})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
