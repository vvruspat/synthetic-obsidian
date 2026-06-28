#!/usr/bin/env python3
"""Build a cleaner note-detector dataset from imperfect audio<->MIDI candidate matches."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import re
import statistics
import wave
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import librosa
import numpy as np
import pretty_midi
import soundfile as sf


@dataclass(frozen=True)
class MidiNote:
    start: float
    end: float
    midi: int
    velocity: int
    lyric: str = ""


@dataclass(frozen=True)
class CandidatePair:
    audio_id: str
    midi_id: str
    audio_path: Path
    midi_path: Path
    confidence: float
    song_score: float
    artist_score: float
    title_score: float
    track_score: float
    audio_vocal_score: float
    midi_vocal_score: float
    audio_file: str
    midi_file: str


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Select, align, and normalize the best audio/MIDI matches into a note-detector dataset."
    )
    parser.add_argument("--candidate-csv", type=Path, required=True, help="CSV of audio<->MIDI candidate matches.")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("data/note_detector/candidate_matches_dataset"),
        help="Output directory for labels, manifest, and summary.",
    )
    parser.add_argument(
        "--min-confidence",
        type=float,
        default=0.76,
        help="Drop candidate rows below this metadata confidence before alignment.",
    )
    parser.add_argument(
        "--max-duration-ratio-delta",
        type=float,
        default=0.28,
        help="Drop pairs when |audio_duration/midi_duration - 1| exceeds this threshold.",
    )
    parser.add_argument(
        "--min-quality-score",
        type=float,
        default=0.52,
        help="Minimum final quality score for keeping an aligned pair.",
    )
    parser.add_argument(
        "--max-pitch-mae",
        type=float,
        default=3.0,
        help="Reject aligned pairs when median pitch error exceeds this many semitones.",
    )
    parser.add_argument(
        "--max-offset-seconds",
        type=float,
        default=6.0,
        help="Reject aligned pairs whose best coarse offset exceeds this absolute value.",
    )
    parser.add_argument(
        "--alignment-hop",
        type=int,
        default=512,
        help="Hop size for alignment features.",
    )
    parser.add_argument(
        "--alignment-sample-rate",
        type=int,
        default=16000,
        help="Sample rate used while scoring alignment.",
    )
    parser.add_argument(
        "--alignment-window-seconds",
        type=float,
        default=120.0,
        help="Only the first N seconds are used for coarse pair scoring.",
    )
    parser.add_argument(
        "--min-note-seconds",
        type=float,
        default=0.03,
        help="Drop MIDI notes shorter than this threshold.",
    )
    parser.add_argument(
        "--split-gap-seconds",
        type=float,
        default=0.35,
        help="Start a new chunk when the gap between kept notes exceeds this threshold.",
    )
    parser.add_argument(
        "--max-chunk-seconds",
        type=float,
        default=10.0,
        help="Maximum duration of a chunk before forced split.",
    )
    parser.add_argument(
        "--max-notes-per-chunk",
        type=int,
        default=40,
        help="Maximum notes in a chunk before forced split.",
    )
    parser.add_argument(
        "--pre-roll-seconds",
        type=float,
        default=0.10,
        help="Audio padding added before the first chunk note.",
    )
    parser.add_argument(
        "--post-roll-seconds",
        type=float,
        default=0.16,
        help="Audio padding added after the last chunk note.",
    )
    parser.add_argument(
        "--audio-threshold-db",
        type=float,
        default=-42.0,
        help="Relative peak threshold for first detectable vocal activity.",
    )
    parser.add_argument(
        "--max-pairs",
        type=int,
        default=0,
        help="Optional cap on the number of selected pairs after metadata filtering (0 = no cap).",
    )
    return parser.parse_args()


def deterministic_split(basename: str) -> str:
    bucket = int(hashlib.md5(basename.encode("utf-8")).hexdigest()[:8], 16) % 100
    if bucket < 10:
        return "test"
    if bucket < 20:
        return "val"
    return "train"


def wav_info(path: Path) -> tuple[int, int, float]:
    with wave.open(str(path), "rb") as handle:
        sample_rate = handle.getframerate()
        frames = handle.getnframes()
        duration = frames / sample_rate if sample_rate else 0.0
    return sample_rate, frames, duration


def audio_info(path: Path) -> tuple[int, int, float]:
    if path.suffix.lower() == ".wav":
        return wav_info(path)
    info = sf.info(str(path))
    frame_count = int(info.frames)
    duration = float(info.duration)
    return int(info.samplerate), frame_count, duration


def first_audio_activity(path: Path, threshold_db: float, frame: int = 2048, hop: int = 256) -> float:
    audio, sample_rate = sf.read(str(path), always_2d=False)
    if getattr(audio, "ndim", 1) > 1:
        audio = audio.mean(axis=1)
    if len(audio) == 0:
        return 0.0
    peak = float(np.max(np.abs(audio)))
    if peak <= 1.0e-9:
        return 0.0

    threshold = peak * (10 ** (threshold_db / 20.0))
    limit = max(1, len(audio) - frame)
    for start in range(0, limit, hop):
        if float(np.max(np.abs(audio[start : start + frame]))) >= threshold:
            return start / sample_rate
    return 0.0


def load_notes(path: Path, min_note_seconds: float) -> list[MidiNote]:
    midi = pretty_midi.PrettyMIDI(str(path))
    notes = sorted((note for instrument in midi.instruments for note in instrument.notes), key=lambda note: note.start)
    output: list[MidiNote] = []
    for note in notes:
        duration = note.end - note.start
        if duration < min_note_seconds:
            continue
        output.append(MidiNote(float(note.start), float(note.end), int(note.pitch), int(note.velocity)))
    return output


def split_note_chunks(notes: list[MidiNote], split_gap_seconds: float, max_chunk_seconds: float, max_notes_per_chunk: int) -> list[list[MidiNote]]:
    if not notes:
        return []

    chunks: list[list[MidiNote]] = []
    current: list[MidiNote] = [notes[0]]
    current_start = notes[0].start

    for previous, note in zip(notes, notes[1:]):
        gap = max(0.0, note.start - previous.end)
        chunk_duration = note.end - current_start
        should_split = (
            gap >= split_gap_seconds
            or chunk_duration > max_chunk_seconds
            or len(current) >= max_notes_per_chunk
        )
        if should_split:
            chunks.append(current)
            current = [note]
            current_start = note.start
        else:
            current.append(note)

    if current:
        chunks.append(current)
    return chunks


def build_label_payload(
    basename: str,
    wav_path: Path,
    midi_path: Path,
    notes: list[MidiNote],
    sample_rate: int,
    frame_count: int,
    duration: float,
    audio_activity_start: float,
    quality: dict[str, Any],
) -> dict[str, Any]:
    first_midi_start = notes[0].start if notes else 0.0
    last_midi_end = notes[-1].end if notes else 0.0
    return {
        "schema": "synthetic-obsidian.note-detector.candidate-match.v1",
        "source": "candidate_matches_csv",
        "id": basename,
        "audio": str(wav_path),
        "midi": str(midi_path),
        "split": deterministic_split(basename),
        "audio_info": {
            "sample_rate": sample_rate,
            "frame_count": frame_count,
            "duration_seconds": duration,
            "channels": sf.info(str(wav_path)).channels,
        },
        "alignment": {
            "audio_activity_start": audio_activity_start,
            "first_midi_note_start": first_midi_start,
            "last_midi_note_end": last_midi_end,
            "midi_minus_audio_activity": first_midi_start - audio_activity_start,
            **quality,
        },
        "notes": [note.__dict__ for note in notes],
    }


def build_manifest_rows(
    basename: str,
    label_path: Path,
    wav_path: Path,
    notes: list[MidiNote],
    duration: float,
    split_gap_seconds: float,
    max_chunk_seconds: float,
    max_notes_per_chunk: int,
    pre_roll_seconds: float,
    post_roll_seconds: float,
) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    indexed_notes = {id(note): index for index, note in enumerate(notes)}
    for index, chunk in enumerate(
        split_note_chunks(notes, split_gap_seconds, max_chunk_seconds, max_notes_per_chunk)
    ):
        chunk_start = max(0.0, chunk[0].start - pre_roll_seconds)
        chunk_end = min(duration, chunk[-1].end + post_roll_seconds)
        if chunk_end <= chunk_start:
            continue
        rows.append(
            {
                "id": f"{basename}_chunk_{index:03d}",
                "source_id": basename,
                "audio": str(wav_path),
                "label": str(label_path),
                "split": deterministic_split(basename),
                "start": chunk_start,
                "end": chunk_end,
                "duration_seconds": chunk_end - chunk_start,
                "note_count": len(chunk),
                "midi_start": chunk[0].start,
                "midi_end": chunk[-1].end,
                "notes_range": [indexed_notes[id(chunk[0])], indexed_notes[id(chunk[-1])]],
            }
        )
    return rows


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def normalize_text(value: str) -> str:
    value = value.casefold()
    value = re.sub(r"[\W_]+", " ", value, flags=re.UNICODE)
    return " ".join(value.split())


def role_score(name: str) -> float:
    text = normalize_text(name)
    positive = (
        "lead",
        "vocal",
        "vocals",
        "vox",
        "voice",
        "voce",
        "lyric",
        "lyrics",
        "verse",
        "main",
        "melody",
    )
    negative = (
        "back",
        "backing",
        "chorus",
        "double",
        "dub",
        "harmony",
        "harm",
        "synth",
        "guide",
        "scratch",
        "demo",
        "gang",
        "crowd",
        "fx",
        "effect",
    )
    score = 0.0
    for token in positive:
        if token in text:
            score += 1.0
    for token in negative:
        if token in text:
            score -= 1.5
    return score


def candidate_from_row(row: dict[str, str]) -> CandidatePair:
    return CandidatePair(
        audio_id=row["audio_id"],
        midi_id=row["midi_id"],
        audio_path=Path(row["audio_path"]),
        midi_path=Path(row["midi_path"]),
        confidence=float(row["confidence"]),
        song_score=float(row["song_score"]),
        artist_score=float(row["artist_score"]),
        title_score=float(row["title_score"]),
        track_score=float(row["track_score"]),
        audio_vocal_score=float(row["audio_vocal_score"]),
        midi_vocal_score=float(row["midi_vocal_score"]),
        audio_file=row["audio_file"],
        midi_file=row["midi_file"],
    )


def metadata_pair_score(candidate: CandidatePair, audio_duration: float, midi_duration: float) -> float:
    duration_ratio_delta = abs((audio_duration / max(midi_duration, 1.0e-6)) - 1.0)
    duration_score = max(0.0, 1.0 - min(duration_ratio_delta, 1.0))
    role_audio = role_score(candidate.audio_file)
    role_midi = role_score(candidate.midi_file)
    role_total = role_audio + role_midi
    score = (
        0.40 * candidate.confidence
        + 0.15 * candidate.track_score
        + 0.10 * duration_score
        + 0.10 * min(candidate.song_score, candidate.artist_score)
        + 0.10 * candidate.title_score
        + 0.08 * np.clip(candidate.audio_vocal_score / 10.0, 0.0, 1.4)
        + 0.07 * np.clip(candidate.midi_vocal_score / 10.0, -0.3, 1.4)
    )
    score += 0.05 * np.clip(role_total, -2.0, 2.0)
    return float(score)


def midi_feature_curves(
    notes: list[MidiNote],
    frame_count: int,
    hop_seconds: float,
    offset_seconds: float = 0.0,
) -> tuple[np.ndarray, np.ndarray]:
    onset = np.zeros(frame_count, dtype=np.float32)
    pitch = np.full(frame_count, np.nan, dtype=np.float32)
    for note in notes:
        start_frame = int(round((note.start + offset_seconds) / hop_seconds))
        end_frame = int(round((note.end + offset_seconds) / hop_seconds))
        if end_frame < 0 or start_frame >= frame_count:
            continue
        start_frame = max(0, start_frame)
        end_frame = min(frame_count - 1, end_frame)
        onset[start_frame] += 1.0
        pitch[start_frame : end_frame + 1] = float(note.midi)
    return onset, pitch


def smooth_impulses(values: np.ndarray) -> np.ndarray:
    if values.size == 0:
        return values
    kernel = np.asarray([0.12, 0.28, 0.72, 1.0, 0.72, 0.28, 0.12], dtype=np.float32)
    return np.convolve(values, kernel, mode="same")


def coarse_offset_search(audio_onset: np.ndarray, notes: list[MidiNote], hop_seconds: float, max_offset_seconds: float) -> tuple[float, float]:
    frame_count = len(audio_onset)
    if frame_count == 0:
        return 0.0, 0.0

    audio_curve = smooth_impulses(audio_onset.astype(np.float32))
    audio_curve = audio_curve - float(np.mean(audio_curve))
    audio_norm = float(np.linalg.norm(audio_curve)) + 1.0e-6

    best_offset = 0.0
    best_score = -1.0
    offsets = np.arange(-max_offset_seconds, max_offset_seconds + hop_seconds, hop_seconds, dtype=np.float32)
    for offset in offsets:
        midi_onset, _ = midi_feature_curves(notes, frame_count, hop_seconds, float(offset))
        midi_curve = smooth_impulses(midi_onset)
        if not np.any(midi_curve):
            continue
        midi_curve = midi_curve - float(np.mean(midi_curve))
        denom = audio_norm * (float(np.linalg.norm(midi_curve)) + 1.0e-6)
        score = float(np.dot(audio_curve, midi_curve) / denom)
        if score > best_score:
            best_score = score
            best_offset = float(offset)
    return best_offset, max(0.0, best_score)


def median_pitch_error(audio_midi: np.ndarray, midi_pitch: np.ndarray) -> float | None:
    valid = np.isfinite(audio_midi) & np.isfinite(midi_pitch)
    if int(np.sum(valid)) < 24:
        return None
    return float(np.median(np.abs(audio_midi[valid] - midi_pitch[valid])))


def voiced_overlap(audio_midi: np.ndarray, midi_pitch: np.ndarray) -> float:
    audio_voiced = np.isfinite(audio_midi)
    midi_voiced = np.isfinite(midi_pitch)
    if not np.any(audio_voiced):
        return 0.0
    return float(np.sum(audio_voiced & midi_voiced) / np.sum(audio_voiced))


def alignment_metrics(
    audio_path: Path,
    notes: list[MidiNote],
    sample_rate: int,
    hop_length: int,
    analysis_window_seconds: float,
) -> dict[str, float | None]:
    audio, _sample_rate = librosa.load(str(audio_path), sr=sample_rate, mono=True, duration=analysis_window_seconds)
    if audio.size == 0:
        return {
            "best_offset_seconds": 0.0,
            "onset_correlation": 0.0,
            "pitch_mae_semitones": None,
            "voiced_overlap": 0.0,
        }

    onset = librosa.onset.onset_strength(y=audio, sr=sample_rate, hop_length=hop_length, aggregate=np.median).astype(np.float32)
    if onset.size and float(np.max(onset)) > 1.0e-6:
        onset = onset / float(np.max(onset))
    hop_seconds = hop_length / sample_rate
    best_offset, onset_corr = coarse_offset_search(onset, notes, hop_seconds, max_offset_seconds=6.0)

    f0, voiced_flag, _voiced_prob = librosa.pyin(
        audio,
        sr=sample_rate,
        hop_length=hop_length,
        fmin=librosa.note_to_hz("C2"),
        fmax=librosa.note_to_hz("C7"),
        frame_length=2048,
    )
    if f0 is None:
        return {
            "best_offset_seconds": best_offset,
            "onset_correlation": onset_corr,
            "pitch_mae_semitones": None,
            "voiced_overlap": 0.0,
        }

    audio_midi = librosa.hz_to_midi(f0)
    audio_midi = np.asarray(audio_midi, dtype=np.float32)
    audio_midi[~np.isfinite(audio_midi)] = np.nan
    if voiced_flag is not None:
        voiced_mask = np.asarray(voiced_flag, dtype=bool)
        audio_midi[~voiced_mask] = np.nan

    _, midi_pitch = midi_feature_curves(notes, len(audio_midi), hop_seconds, offset_seconds=best_offset)
    pitch_mae = median_pitch_error(audio_midi, midi_pitch)
    overlap = voiced_overlap(audio_midi, midi_pitch)
    return {
        "best_offset_seconds": best_offset,
        "onset_correlation": onset_corr,
        "pitch_mae_semitones": pitch_mae,
        "voiced_overlap": overlap,
    }


def quality_score(metrics: dict[str, float | None], metadata_score: float) -> float:
    onset = float(metrics["onset_correlation"] or 0.0)
    overlap = float(metrics["voiced_overlap"] or 0.0)
    pitch_mae = metrics["pitch_mae_semitones"]
    pitch_score = 0.0 if pitch_mae is None else max(0.0, 1.0 - min(float(pitch_mae), 12.0) / 12.0)
    return float(0.35 * metadata_score + 0.30 * onset + 0.20 * overlap + 0.15 * pitch_score)


def choose_unique_candidates(rows: list[CandidatePair], metadata_scores: dict[tuple[str, str], float]) -> list[CandidatePair]:
    chosen_audio: set[str] = set()
    chosen_midi: set[str] = set()
    output: list[CandidatePair] = []
    for candidate in sorted(rows, key=lambda item: metadata_scores[(item.audio_id, item.midi_id)], reverse=True):
        if candidate.audio_id in chosen_audio or candidate.midi_id in chosen_midi:
            continue
        chosen_audio.add(candidate.audio_id)
        chosen_midi.add(candidate.midi_id)
        output.append(candidate)
    return output


def stats_dict(values: list[float] | list[int]) -> dict[str, float] | None:
    if not values:
        return None
    ordered = sorted(float(value) for value in values)
    return {
        "min": ordered[0],
        "median": statistics.median(ordered),
        "mean": statistics.mean(ordered),
        "p90": ordered[min(len(ordered) - 1, int(0.9 * (len(ordered) - 1)))],
        "max": ordered[-1],
    }


def main() -> int:
    args = parse_args()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    label_dir = output_dir / "labels"

    with args.candidate_csv.open("r", encoding="utf-8", newline="") as handle:
        raw_rows = [candidate_from_row(row) for row in csv.DictReader(handle)]

    warnings: list[str] = []
    metadata_scores: dict[tuple[str, str], float] = {}
    audio_meta: dict[str, tuple[int, int, float]] = {}
    midi_durations: dict[str, float] = {}
    filtered_rows: list[CandidatePair] = []

    for row in raw_rows:
        if row.confidence < args.min_confidence:
            continue
        if not row.audio_path.exists() or not row.midi_path.exists():
            warnings.append(f"missing file: {row.audio_path} || {row.midi_path}")
            continue
        try:
            sample_rate, frame_count, audio_duration = audio_info(row.audio_path)
            midi_duration = pretty_midi.PrettyMIDI(str(row.midi_path)).get_end_time()
        except Exception as exc:
            warnings.append(f"metadata failed: {row.audio_path.name} || {row.midi_path.name} ({exc})")
            continue
        if midi_duration <= 0.0 or audio_duration <= 0.0:
            continue
        duration_ratio_delta = abs((audio_duration / midi_duration) - 1.0)
        if duration_ratio_delta > args.max_duration_ratio_delta:
            continue
        score = metadata_pair_score(row, audio_duration, midi_duration)
        metadata_scores[(row.audio_id, row.midi_id)] = score
        audio_meta[row.audio_id] = (sample_rate, frame_count, audio_duration)
        midi_durations[row.midi_id] = midi_duration
        filtered_rows.append(row)

    selected = choose_unique_candidates(filtered_rows, metadata_scores)
    if args.max_pairs > 0:
        selected = selected[: args.max_pairs]

    manifest_rows: list[dict[str, Any]] = []
    selected_quality: list[float] = []
    onset_scores: list[float] = []
    pitch_maes: list[float] = []
    overlap_scores: list[float] = []
    offsets: list[float] = []
    audio_durations: list[float] = []
    note_counts: list[int] = []
    chunk_durations: list[float] = []

    kept_pairs = 0
    for index, candidate in enumerate(selected):
        try:
            notes = load_notes(candidate.midi_path, args.min_note_seconds)
            sample_rate, frame_count, audio_duration = audio_info(candidate.audio_path)
            if not notes:
                warnings.append(f"no notes: {candidate.audio_path.name} || {candidate.midi_path.name}")
                continue
            metrics = alignment_metrics(
                candidate.audio_path,
                notes,
                sample_rate=args.alignment_sample_rate,
                hop_length=args.alignment_hop,
                analysis_window_seconds=args.alignment_window_seconds,
            )
            quality = quality_score(metrics, metadata_scores[(candidate.audio_id, candidate.midi_id)])
            pitch_mae = metrics["pitch_mae_semitones"]
            best_offset = float(metrics["best_offset_seconds"] or 0.0)
            if abs(best_offset) > args.max_offset_seconds:
                continue
            if pitch_mae is not None and pitch_mae > args.max_pitch_mae:
                continue
            if quality < args.min_quality_score:
                continue

            shifted_notes = [
                MidiNote(
                    start=max(0.0, note.start + best_offset),
                    end=max(0.0, note.end + best_offset),
                    midi=note.midi,
                    velocity=note.velocity,
                    lyric=note.lyric,
                )
                for note in notes
                if note.end + best_offset > 0.0
            ]
            shifted_notes = [note for note in shifted_notes if note.end - note.start >= args.min_note_seconds]
            shifted_notes.sort(key=lambda note: (note.start, note.end, note.midi))
            if not shifted_notes:
                continue

            audio_activity_start = first_audio_activity(candidate.audio_path, args.audio_threshold_db)
            source_name = f"pair_{index:04d}_{candidate.audio_path.stem}"
            quality_payload = {
                "metadata_score": metadata_scores[(candidate.audio_id, candidate.midi_id)],
                "pair_quality_score": quality,
                **metrics,
                "audio_id": candidate.audio_id,
                "midi_id": candidate.midi_id,
            }
            label_payload = build_label_payload(
                basename=source_name,
                wav_path=candidate.audio_path,
                midi_path=candidate.midi_path,
                notes=shifted_notes,
                sample_rate=sample_rate,
                frame_count=frame_count,
                duration=audio_duration,
                audio_activity_start=audio_activity_start,
                quality=quality_payload,
            )
            label_path = label_dir / f"{source_name}.json"
            write_json(label_path, label_payload)

            rows = build_manifest_rows(
                basename=source_name,
                label_path=label_path,
                wav_path=candidate.audio_path,
                notes=shifted_notes,
                duration=audio_duration,
                split_gap_seconds=args.split_gap_seconds,
                max_chunk_seconds=args.max_chunk_seconds,
                max_notes_per_chunk=args.max_notes_per_chunk,
                pre_roll_seconds=args.pre_roll_seconds,
                post_roll_seconds=args.post_roll_seconds,
            )
            if not rows:
                continue

            manifest_rows.extend(rows)
            kept_pairs += 1
            selected_quality.append(quality)
            onset_scores.append(float(metrics["onset_correlation"] or 0.0))
            overlap_scores.append(float(metrics["voiced_overlap"] or 0.0))
            offsets.append(best_offset)
            if pitch_mae is not None:
                pitch_maes.append(float(pitch_mae))
            audio_durations.append(audio_duration)
            note_counts.append(len(shifted_notes))
            chunk_durations.extend(row["duration_seconds"] for row in rows)
        except Exception as exc:
            warnings.append(f"pair failed: {candidate.audio_path.name} || {candidate.midi_path.name} ({exc})")

    manifest_path = output_dir / "manifest.jsonl"
    with manifest_path.open("w", encoding="utf-8") as handle:
        for row in manifest_rows:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")

    summary = {
        "schema": "synthetic-obsidian.note-detector.candidate-match-summary.v1",
        "candidate_csv": str(args.candidate_csv.resolve()),
        "raw_rows": len(raw_rows),
        "metadata_filtered_rows": len(filtered_rows),
        "selected_unique_pairs": len(selected),
        "kept_pairs": kept_pairs,
        "manifest_rows": len(manifest_rows),
        "audio_minutes": sum(audio_durations) / 60.0 if audio_durations else 0.0,
        "quality_score_stats": stats_dict(selected_quality),
        "onset_correlation_stats": stats_dict(onset_scores),
        "pitch_mae_stats": stats_dict(pitch_maes),
        "voiced_overlap_stats": stats_dict(overlap_scores),
        "offset_stats": stats_dict(offsets),
        "note_count_stats": stats_dict(note_counts),
        "chunk_duration_stats": stats_dict(chunk_durations),
        "warnings_count": len(warnings),
    }
    write_json(output_dir / "summary.json", summary)
    if warnings:
        (output_dir / "warnings.txt").write_text("\n".join(warnings) + "\n", encoding="utf-8")

    print(f"Raw rows: {len(raw_rows)}")
    print(f"Metadata-filtered rows: {len(filtered_rows)}")
    print(f"Selected unique pairs: {len(selected)}")
    print(f"Kept pairs: {kept_pairs}")
    print(f"Manifest rows: {len(manifest_rows)}")
    print(f"Output: {output_dir}")
    if warnings:
        print(f"Warnings: {len(warnings)} ({output_dir / 'warnings.txt'})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
