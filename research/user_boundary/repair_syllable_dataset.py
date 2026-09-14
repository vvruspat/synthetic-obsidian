#!/usr/bin/env python3
"""Repair full-song pseudo syllable labels without treating uncertainty as silence."""

from __future__ import annotations

import argparse
import hashlib
import itertools
import json
import os
import re
import shutil
import subprocess
import tempfile
import unicodedata
from collections import Counter
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

import librosa
import numpy as np
import soundfile as sf


EVENT_NAMES = ("phoneme", "syllable", "breath", "silence")
TRUSTED_MATCH_TYPES = frozenset({"word-anchor+ctc", "ngram-anchor+ctc"})
STRUCTURAL_WORDS = frozenset(
    {
        "bridge",
        "chorus",
        "final",
        "hook",
        "instrumental",
        "interlude",
        "intro",
        "outro",
        "prechorus",
        "refrain",
        "verse",
    }
)
# eSpeak's mnemonic alphabet. Multi-character diphthongs must be consumed first.
VOWEL_NUCLEUS = re.compile(
    r"eI|aI|OI|oU|@U|aU|I@|e@|U@|o@|O@|E@|"
    r"[iIeE{aAQ0oOUuV3@yY](?::)?"
)


@dataclass(frozen=True)
class RepairConfig:
    minimum_confidence: float
    minimum_boundary_distance: float
    interval_merge_gap: float
    allow_acoustic_multisyllable_collapse: bool


@dataclass(frozen=True)
class AcceptedWord:
    word: str
    word_index: int
    start: float
    end: float
    expected_syllables: int
    boundaries: tuple[float, ...]
    source_segment_ids: tuple[int, ...]
    mean_confidence: float
    match_types: tuple[str, ...]


class PhoneticSyllableCounter:
    def __init__(self, executable: str):
        self.executable = executable
        self.cache: dict[tuple[str, str], tuple[int, str]] = {}

    def count(self, word: str, language: str) -> tuple[int, str]:
        voice = "ru" if language.lower().startswith("ru") else "en-us"
        key = (voice, word)
        if key not in self.cache:
            process = subprocess.run(
                [self.executable, "-q", "-x", "-v", voice, word],
                check=True,
                capture_output=True,
                text=True,
            )
            mnemonic = process.stdout.strip()
            self.cache[key] = (
                count_mnemonic_syllables(mnemonic),
                mnemonic,
            )
        return self.cache[key]


class AcousticBoundaryScorer:
    def __init__(self, audio_path: Path):
        self.audio_path = audio_path
        self._strength: np.ndarray | None = None
        self._frames_per_second = 100.0

    def score(self, time_seconds: float) -> float:
        self._ensure_loaded()
        assert self._strength is not None
        frame = int(round(time_seconds * self._frames_per_second))
        start = max(0, frame - 4)
        end = min(len(self._strength), frame + 5)
        if end <= start:
            return 0.0
        return float(np.max(self._strength[start:end]))

    def _ensure_loaded(self) -> None:
        if self._strength is not None:
            return
        audio, _ = librosa.load(
            self.audio_path,
            sr=16_000,
            mono=True,
        )
        hop_length = 160
        onset = librosa.onset.onset_strength(
            y=audio,
            sr=16_000,
            hop_length=hop_length,
            n_fft=512,
        )
        rms = librosa.feature.rms(
            y=audio,
            frame_length=512,
            hop_length=hop_length,
        )[0]
        frame_count = min(len(onset), len(rms))
        onset = np.log1p(onset[:frame_count])
        positive_rms_delta = np.maximum(
            0.0,
            np.diff(rms[:frame_count], prepend=rms[0]),
        )
        onset = robust_unit_scale(onset)
        positive_rms_delta = robust_unit_scale(positive_rms_delta)
        self._strength = onset + 0.35 * positive_rms_delta


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--minimum-confidence", type=float, default=0.8)
    parser.add_argument(
        "--minimum-boundary-distance-ms",
        type=float,
        default=60.0,
    )
    parser.add_argument(
        "--interval-merge-gap-ms",
        type=float,
        default=15.0,
    )
    parser.add_argument(
        "--allow-acoustic-multisyllable-collapse",
        action="store_true",
        help=(
            "opt in to selecting internal boundaries acoustically when an "
            "oversegmented word has multiple real syllables"
        ),
    )
    parser.add_argument("--espeak", type=Path)
    return parser.parse_args()


def normalize_word(value: Any) -> str:
    text = unicodedata.normalize("NFKC", str(value or "")).casefold()
    return re.sub(r"[^\w'’]+", "", text, flags=re.UNICODE).replace("_", "")


def is_structural_word(word: str) -> bool:
    normalized = word.replace("-", "")
    if normalized in STRUCTURAL_WORDS:
        return True
    return bool(re.fullmatch(r"(?:verse|chorus|bridge|intro|outro)\d+", normalized))


def comparable_spelling(value: Any) -> str:
    return normalize_word(value).replace("'", "").replace("’", "")


def count_mnemonic_syllables(mnemonic: str) -> int:
    return len(VOWEL_NUCLEUS.findall(mnemonic))


def robust_unit_scale(values: np.ndarray) -> np.ndarray:
    if values.size == 0:
        return values.astype(np.float32)
    low, high = np.percentile(values, (20.0, 95.0))
    if high <= low + 1.0e-8:
        return np.zeros_like(values, dtype=np.float32)
    return np.clip((values - low) / (high - low), 0.0, 1.5).astype(
        np.float32
    )


def group_segments(
    segments: list[dict[str, Any]],
) -> list[list[dict[str, Any]]]:
    groups: list[list[dict[str, Any]]] = []
    for segment in segments:
        key = (
            int(segment.get("word_index", -1)),
            normalize_word(segment.get("word")),
        )
        if not groups:
            groups.append([segment])
            continue
        previous = groups[-1][-1]
        previous_key = (
            int(previous.get("word_index", -1)),
            normalize_word(previous.get("word")),
        )
        if key == previous_key:
            groups[-1].append(segment)
        else:
            groups.append([segment])
    return groups


def choose_boundaries(
    segments: list[dict[str, Any]],
    expected_count: int,
    minimum_distance: float,
    acoustic_score: Callable[[float], float],
) -> tuple[float, ...] | None:
    starts = [float(segment["start"]) for segment in segments]
    group_end = max(float(segment["end"]) for segment in segments)
    if expected_count <= 0 or len(starts) < expected_count:
        return None
    if expected_count == len(starts):
        selected = tuple(starts)
        return (
            selected
            if valid_boundary_spacing(selected, group_end, minimum_distance)
            else None
        )
    if expected_count == 1:
        return (starts[0],)

    first = starts[0]
    group_duration = max(group_end - first, minimum_distance)
    best: tuple[float, tuple[float, ...]] | None = None
    for indices in itertools.combinations(
        range(1, len(starts)),
        expected_count - 1,
    ):
        selected = (first,) + tuple(starts[index] for index in indices)
        if not valid_boundary_spacing(selected, group_end, minimum_distance):
            continue
        score = 0.0
        for syllable_index, segment_index in enumerate(indices, start=1):
            time_seconds = starts[segment_index]
            ideal = first + group_duration * syllable_index / expected_count
            confidence = float(segments[segment_index].get("confidence", 0.0))
            score += acoustic_score(time_seconds)
            score += 0.1 * confidence
            score -= 0.12 * abs(time_seconds - ideal) / group_duration
        candidate = (score, selected)
        if best is None or candidate > best:
            best = candidate
    return None if best is None else best[1]


def valid_boundary_spacing(
    boundaries: tuple[float, ...],
    group_end: float,
    minimum_distance: float,
) -> bool:
    if not boundaries or boundaries[-1] >= group_end:
        return False
    points = boundaries + (group_end,)
    return all(
        second - first >= minimum_distance
        for first, second in zip(points, points[1:])
    )


def merge_intervals(
    intervals: list[list[float]],
    maximum_gap: float,
) -> list[list[float]]:
    if not intervals:
        return []
    ordered = sorted(
        ([float(start), float(end)] for start, end in intervals),
        key=lambda interval: interval[0],
    )
    merged = [ordered[0]]
    for start, end in ordered[1:]:
        if start <= merged[-1][1] + maximum_gap:
            merged[-1][1] = max(merged[-1][1], end)
        else:
            merged.append([start, end])
    return [[round(start, 6), round(end, 6)] for start, end in merged]


def stable_split(group_key: str) -> str:
    bucket = int(hashlib.sha1(group_key.encode()).hexdigest()[:8], 16) % 100
    return "validation" if bucket < 15 else "train"


def reject_reason(
    group: list[dict[str, Any]],
    word: str,
    expected_count: int,
    duration: float,
    config: RepairConfig,
) -> str | None:
    if not word:
        return "unassigned"
    if is_structural_word(word):
        return "structural_tag"
    if "".join(
        comparable_spelling(segment.get("syllable")) for segment in group
    ) != comparable_spelling(word):
        return "incomplete_word_spelling"
    if expected_count <= 0:
        return "no_phonetic_syllable"
    if any(bool(segment.get("unassigned")) for segment in group):
        return "unassigned"
    if any(
        str(segment.get("match_type", "")) not in TRUSTED_MATCH_TYPES
        for segment in group
    ):
        return "untrusted_alignment"
    if any(
        float(segment.get("confidence", 0.0)) < config.minimum_confidence
        for segment in group
    ):
        return "low_confidence"
    if any(
        float(segment.get("end", 0.0)) <= float(segment.get("start", 0.0))
        or float(segment.get("start", -1.0)) < 0.0
        or float(segment.get("end", duration + 1.0)) > duration + 0.01
        for segment in group
    ):
        return "invalid_timing"
    if len(group) < expected_count:
        return "missing_syllable_boundary"
    if (
        len(group) > expected_count
        and expected_count > 1
        and not config.allow_acoustic_multisyllable_collapse
    ):
        return "ambiguous_internal_boundary"
    return None


def repair_song(
    annotation_path: Path,
    audio_path: Path,
    counter: PhoneticSyllableCounter,
    config: RepairConfig,
) -> tuple[dict[str, Any], dict[str, Any]]:
    source = json.loads(annotation_path.read_text(encoding="utf-8"))
    duration = min(
        float(source["audio"]["duration"]),
        float(sf.info(audio_path).duration),
    )
    language = str(source.get("alignment", {}).get("language", "en"))
    scorer = AcousticBoundaryScorer(audio_path)
    accepted: list[AcceptedWord] = []
    rejected = Counter()
    collapsed_boundaries = 0

    for group in group_segments(list(source["alignment"].get("segments", []))):
        word = normalize_word(group[0].get("word"))
        expected_count, _mnemonic = (
            counter.count(word, language) if word else (0, "")
        )
        reason = reject_reason(group, word, expected_count, duration, config)
        if reason is not None:
            rejected[reason] += 1
            continue
        boundaries = choose_boundaries(
            group,
            expected_count,
            config.minimum_boundary_distance,
            scorer.score,
        )
        if boundaries is None:
            rejected["implausible_boundary_spacing"] += 1
            continue
        collapsed_boundaries += len(group) - len(boundaries)
        accepted.append(
            AcceptedWord(
                word=word,
                word_index=int(group[0].get("word_index", -1)),
                start=min(float(segment["start"]) for segment in group),
                end=max(float(segment["end"]) for segment in group),
                expected_syllables=expected_count,
                boundaries=boundaries,
                source_segment_ids=tuple(
                    int(segment.get("id", -1)) for segment in group
                ),
                mean_confidence=float(
                    np.mean(
                        [
                            float(segment.get("confidence", 0.0))
                            for segment in group
                        ]
                    )
                ),
                match_types=tuple(
                    sorted(
                        {
                            str(segment.get("match_type", ""))
                            for segment in group
                        }
                    )
                ),
            )
        )

    intervals = merge_intervals(
        [[word.start, word.end] for word in accepted],
        config.interval_merge_gap,
    )
    events = [
        {
            "time": round(boundary, 6),
            "word": word.word,
            "word_index": word.word_index,
            "syllable_index": index,
            "syllable_count": word.expected_syllables,
            "source_segment_ids": list(word.source_segment_ids),
            "mean_confidence": round(word.mean_confidence, 4),
            "match_types": list(word.match_types),
        }
        for word in accepted
        for index, boundary in enumerate(word.boundaries)
    ]
    trusted_duration = sum(end - start for start, end in intervals)
    repaired = {
        "schema": "synthetic-obsidian.corrected-syllable-labels.v1",
        "source_annotation": str(annotation_path),
        "audio": audio_path.name,
        "language": language,
        "duration_seconds": round(duration, 6),
        "syllable_events": events,
        "supervision_intervals": intervals,
        "repair": {
            "minimum_confidence": config.minimum_confidence,
            "trusted_match_types": sorted(TRUSTED_MATCH_TYPES),
            "minimum_boundary_distance_ms": (
                config.minimum_boundary_distance * 1000.0
            ),
            "phonetic_counter": "eSpeak mnemonic vowel nuclei",
            "oversegmented_word_policy": (
                "collapse one-syllable words; reject ambiguous multisyllable words"
                if not config.allow_acoustic_multisyllable_collapse
                else "retain the acoustically strongest candidate subset"
            ),
            "accepted_words": len(accepted),
            "accepted_syllable_events": len(events),
            "collapsed_extra_boundaries": collapsed_boundaries,
            "trusted_duration_seconds": round(trusted_duration, 6),
            "rejected_words": sum(rejected.values()),
            "rejection_reasons": dict(sorted(rejected.items())),
        },
    }
    stats = {
        "accepted_words": len(accepted),
        "accepted_syllable_events": len(events),
        "collapsed_extra_boundaries": collapsed_boundaries,
        "trusted_duration_seconds": trusted_duration,
        "rejected_words": sum(rejected.values()),
        "rejection_reasons": rejected,
    }
    return repaired, stats


def build_manifest_record(
    repaired: dict[str, Any],
    source_annotation: dict[str, Any],
    audio_relative: str,
    annotation_relative: str,
    audio_path: Path,
) -> dict[str, Any]:
    info = sf.info(audio_path)
    song_name = audio_path.stem
    group_key = f"full-song/{song_name}"
    syllables = [
        float(event["time"]) for event in repaired["syllable_events"]
    ]
    events = {
        "phoneme": [],
        "syllable": syllables,
        "breath": [],
        "silence": [],
    }
    return {
        "manifest_version": 1,
        "id": hashlib.sha256(group_key.encode()).hexdigest()[:16],
        "audio": audio_relative,
        "annotation": annotation_relative,
        "split": stable_split(group_key),
        "group_key": group_key,
        "language": repaired["language"],
        "singer": "synthetic_full_song",
        "technique": "mixed",
        "song": song_name,
        "group": "corrected_syllable_pseudo_labels",
        "clip": song_name,
        "sample_rate": int(info.samplerate),
        "channels": int(info.channels),
        "num_frames": int(info.frames),
        "duration_seconds": round(float(info.duration), 6),
        "events": events,
        "intervals": {"breath": [], "silence": []},
        "event_counts": {name: len(events[name]) for name in EVENT_NAMES},
        "loss_heads": ["syllable"],
        "supervision_intervals": repaired["supervision_intervals"],
        "label_notes": {
            "source": "repaired full-song forced-alignment pseudo-labels",
            "syllable": (
                "phonetic syllable count checked with eSpeak; excess "
                "orthographic boundaries collapsed using acoustic onset evidence"
            ),
            "negative_frames": (
                "only frames inside supervision_intervals are valid negatives"
            ),
            "breath": "not annotated by this dataset",
            "silence": "not annotated by this dataset",
        },
        "source_song_id": source_annotation.get("song_id"),
    }


def locate_espeak(requested: Path | None) -> str:
    if requested is not None:
        executable = requested.expanduser().resolve()
        if not executable.is_file():
            raise FileNotFoundError(f"eSpeak executable does not exist: {executable}")
        return str(executable)
    discovered = shutil.which("espeak-ng") or shutil.which("espeak")
    if discovered is None:
        raise RuntimeError("eSpeak is required to repair phonetic syllable counts")
    return discovered


def validate_arguments(args: argparse.Namespace) -> tuple[Path, Path, RepairConfig]:
    input_dir = args.input_dir.expanduser().resolve()
    output_dir = args.output_dir.expanduser().resolve()
    if not input_dir.is_dir():
        raise FileNotFoundError(f"input directory does not exist: {input_dir}")
    if output_dir.exists():
        raise FileExistsError(
            f"output path already exists; refusing to overwrite: {output_dir}"
        )
    if not 0.0 <= args.minimum_confidence <= 1.0:
        raise ValueError("minimum confidence must be between zero and one")
    if args.minimum_boundary_distance_ms <= 0.0:
        raise ValueError("minimum boundary distance must be positive")
    if args.interval_merge_gap_ms < 0.0:
        raise ValueError("interval merge gap cannot be negative")
    return (
        input_dir,
        output_dir,
        RepairConfig(
            minimum_confidence=float(args.minimum_confidence),
            minimum_boundary_distance=(
                float(args.minimum_boundary_distance_ms) / 1000.0
            ),
            interval_merge_gap=float(args.interval_merge_gap_ms) / 1000.0,
            allow_acoustic_multisyllable_collapse=bool(
                args.allow_acoustic_multisyllable_collapse
            ),
        ),
    )


def main() -> int:
    args = parse_args()
    input_dir, output_dir, config = validate_arguments(args)
    counter = PhoneticSyllableCounter(locate_espeak(args.espeak))
    annotations = sorted(input_dir.glob("Song*.json"))
    if not annotations:
        raise RuntimeError(f"no Song*.json annotations found in {input_dir}")

    output_dir.parent.mkdir(parents=True, exist_ok=True)
    temporary_dir = Path(
        tempfile.mkdtemp(
            prefix=f".{output_dir.name}.",
            dir=output_dir.parent,
        )
    )
    audio_dir = temporary_dir / "audio"
    annotation_dir = temporary_dir / "annotations"
    audio_dir.mkdir()
    annotation_dir.mkdir()

    records = []
    totals = Counter()
    rejection_reasons = Counter()
    try:
        for index, annotation_path in enumerate(annotations, start=1):
            source_annotation = json.loads(
                annotation_path.read_text(encoding="utf-8")
            )
            audio_name = str(source_annotation["audio"]["file"])
            audio_path = input_dir / audio_name
            if not audio_path.is_file():
                raise FileNotFoundError(f"missing paired audio: {audio_path}")
            repaired, stats = repair_song(
                annotation_path,
                audio_path,
                counter,
                config,
            )
            repaired_name = annotation_path.name
            repaired_path = annotation_dir / repaired_name
            repaired_path.write_text(
                json.dumps(repaired, ensure_ascii=False, indent=2) + "\n",
                encoding="utf-8",
            )
            audio_link = audio_dir / audio_name
            audio_link.symlink_to(os.path.relpath(audio_path, audio_dir))
            record = build_manifest_record(
                repaired,
                source_annotation,
                f"audio/{audio_name}",
                f"annotations/{repaired_name}",
                audio_path,
            )
            records.append(record)
            for name in (
                "accepted_words",
                "accepted_syllable_events",
                "collapsed_extra_boundaries",
                "rejected_words",
            ):
                totals[name] += int(stats[name])
            totals["trusted_duration_milliseconds"] += int(
                round(float(stats["trusted_duration_seconds"]) * 1000.0)
            )
            rejection_reasons.update(stats["rejection_reasons"])
            print(
                f"repaired {index}/{len(annotations)} {audio_name}: "
                f"{stats['accepted_syllable_events']} events",
                flush=True,
            )

        manifest_path = temporary_dir / "manifest.jsonl"
        manifest_path.write_text(
            "".join(
                json.dumps(record, ensure_ascii=False) + "\n"
                for record in records
            ),
            encoding="utf-8",
        )
        source_duration = sum(
            float(record["duration_seconds"]) for record in records
        )
        trusted_duration = totals["trusted_duration_milliseconds"] / 1000.0
        summary = {
            "schema": "synthetic-obsidian.corrected-syllable-dataset.v1",
            "source_dataset": str(input_dir),
            "records": len(records),
            "train_records": sum(
                record["split"] == "train" for record in records
            ),
            "validation_records": sum(
                record["split"] == "validation" for record in records
            ),
            "source_duration_seconds": round(source_duration, 6),
            "trusted_duration_seconds": round(trusted_duration, 6),
            "trusted_audio_fraction": round(
                trusted_duration / source_duration if source_duration else 0.0,
                6,
            ),
            "accepted_words": totals["accepted_words"],
            "accepted_syllable_events": totals["accepted_syllable_events"],
            "collapsed_extra_boundaries": totals[
                "collapsed_extra_boundaries"
            ],
            "rejected_words": totals["rejected_words"],
            "rejection_reasons": dict(sorted(rejection_reasons.items())),
            "configuration": {
                "minimum_confidence": config.minimum_confidence,
                "minimum_boundary_distance_ms": (
                    config.minimum_boundary_distance * 1000.0
                ),
                "interval_merge_gap_ms": config.interval_merge_gap * 1000.0,
                "trusted_match_types": sorted(TRUSTED_MATCH_TYPES),
                "loss_heads": ["syllable"],
                "allow_acoustic_multisyllable_collapse": (
                    config.allow_acoustic_multisyllable_collapse
                ),
            },
        }
        (temporary_dir / "summary.json").write_text(
            json.dumps(summary, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8",
        )
        os.replace(temporary_dir, output_dir)
    except BaseException:
        shutil.rmtree(temporary_dir, ignore_errors=True)
        raise

    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
