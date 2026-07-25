#!/usr/bin/env python3
"""Convert multitrack-per-folder MIDI songs into backing-vocal DSL inputs."""

from __future__ import annotations

import argparse
import math
import re
import statistics
from collections import Counter, defaultdict, deque
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

import mido


NOTE_NAMES = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
MAJOR_PROFILE = (6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88)
MINOR_PROFILE = (6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17)

VOCAL_HINTS = (
    "vocal",
    "vocals",
    "vox",
    "voice",
    "voz",
    "voix",
    "canto",
    "chant",
    "sing",
    "singer",
    "lyrics",
)
BACKING_VOCAL_HINTS = (
    "back",
    "backing",
    "backvocal",
    "backvocals",
    "background",
    "backup",
    "bck",
    "bkup",
    "bg",
    "harmony",
    "choir",
    "chorus",
    "bgv",
    "bvox",
)
NON_VOCAL_HINTS = (
    "drum",
    "bateria",
    "perc",
    "bass",
    "bajo",
    "guitar",
    "guitarra",
    "piano",
    "organ",
    "synth",
    "strings",
    "violin",
    "cymbal",
)


CHORD_TEMPLATES = (
    ("major", "", (0, 4, 7)),
    ("minor", "m", (0, 3, 7)),
    ("dominant7", "7", (0, 4, 7, 10)),
    ("major7", "maj7", (0, 4, 7, 11)),
    ("minor7", "m7", (0, 3, 7, 10)),
    ("diminished", "dim", (0, 3, 6)),
    ("half_diminished7", "m7b5", (0, 3, 6, 10)),
    ("augmented", "aug", (0, 4, 8)),
    ("sus2", "sus2", (0, 2, 7)),
    ("sus4", "sus4", (0, 5, 7)),
    ("sixth", "6", (0, 4, 7, 9)),
    ("minor6", "m6", (0, 3, 7, 9)),
)


@dataclass(frozen=True)
class Note:
    start: float
    duration: float
    pitch: int
    velocity: int
    source: str
    channel: int
    track_name: str = ""
    lyric: str | None = None

    @property
    def end(self) -> float:
        return self.start + self.duration


@dataclass(frozen=True)
class MidiTrackData:
    path: Path
    notes: tuple[Note, ...]
    lyrics: tuple[tuple[float, str], ...]
    ticks_per_beat: int
    tempo: int | None
    meter: str | None
    key: str | None
    names: tuple[str, ...]


@dataclass(frozen=True)
class LeadCandidate:
    source: str
    notes: tuple[Note, ...]
    confidence: float
    reason: str
    has_vocal_hint: bool
    has_lyrics: bool
    is_backing_hint: bool


@dataclass(frozen=True)
class ChordGuess:
    name: str
    root: int
    quality: str
    bass: int
    intervals: tuple[int, ...]
    confidence: float
    alternatives: tuple[str, ...]


@dataclass
class SongResult:
    song_dir: Path
    output_path: Path
    written: bool
    reason: str = ""
    lead_confidence: float = 0.0
    chord_count: int = 0
    lead_note_count: int = 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--midi-root", type=Path, required=True)
    parser.add_argument("--output-root", type=Path, default=None)
    parser.add_argument("--workers", type=int, default=8)
    parser.add_argument("--limit", type=int, default=0)
    parser.add_argument("--min-lead-confidence", type=float, default=0.52)
    parser.add_argument("--allow-heuristic-lead", action="store_true")
    parser.add_argument("--allow-empty-chords", action="store_true")
    parser.add_argument("--overwrite", action="store_true")
    return parser.parse_args()


def midi_note_name(note: int) -> str:
    octave = note // 12 - 1
    return f"{NOTE_NAMES[note % 12]}{octave}"


def plain_note_name(pc: int) -> str:
    return NOTE_NAMES[pc % 12]


def bpm_from_tempo(tempo: int | None) -> int:
    if not tempo:
        return 120
    return int(round(mido.tempo2bpm(tempo)))


def normalize_text(value: str) -> str:
    return value.lower().replace("_", " ").replace("-", " ")


def has_any(text: str, hints: Iterable[str]) -> bool:
    lowered = normalize_text(text)
    tokens = set(re.findall(r"[a-z0-9]+", lowered))
    return any(hint in lowered if " " in hint else hint in tokens for hint in hints)


def read_midi_file(path: Path) -> MidiTrackData:
    midi = mido.MidiFile(path)
    notes: list[Note] = []
    lyrics: list[tuple[float, str]] = []
    names: list[str] = []
    tempos: list[int] = []
    meters: list[str] = []
    keys: list[str] = []

    for index, track in enumerate(midi.tracks):
        abs_tick = 0
        track_name = ""
        active: dict[tuple[int, int], deque[tuple[int, int]]] = defaultdict(deque)

        for message in track:
            abs_tick += message.time
            if message.is_meta:
                if message.type == "track_name":
                    track_name = str(message.name)
                    names.append(track_name)
                elif message.type == "set_tempo":
                    tempos.append(int(message.tempo))
                elif message.type == "time_signature":
                    meters.append(f"{message.numerator}/{message.denominator}")
                elif message.type == "key_signature":
                    keys.append(str(message.key))
                elif message.type in ("lyrics", "text") and getattr(message, "text", "").strip():
                    lyrics.append((abs_tick / midi.ticks_per_beat, message.text.strip()))
                continue

            if message.type == "note_on" and message.velocity > 0:
                active[(message.channel, message.note)].append((abs_tick, message.velocity))
            elif message.type in ("note_off", "note_on"):
                key = (message.channel, message.note)
                if key not in active or not active[key]:
                    continue
                start_tick, velocity = active[key].popleft()
                if abs_tick <= start_tick:
                    continue
                source_name = path.stem
                if track_name:
                    source_name = f"{path.stem}::{track_name}"
                notes.append(
                    Note(
                        start=start_tick / midi.ticks_per_beat,
                        duration=(abs_tick - start_tick) / midi.ticks_per_beat,
                        pitch=message.note,
                        velocity=velocity,
                        source=source_name,
                        channel=message.channel,
                        track_name=track_name or f"track_{index + 1}",
                    )
                )

    notes.sort(key=lambda note: (note.start, note.pitch, note.source))
    lyrics.sort(key=lambda item: item[0])
    return MidiTrackData(
        path=path,
        notes=tuple(notes),
        lyrics=tuple(lyrics),
        ticks_per_beat=midi.ticks_per_beat,
        tempo=tempos[0] if tempos else None,
        meter=meters[0] if meters else None,
        key=keys[0] if keys else None,
        names=tuple(names),
    )


def group_song_dirs(midi_root: Path) -> list[Path]:
    song_dirs: list[Path] = []
    for directory in midi_root.rglob("*"):
        if not directory.is_dir():
            continue
        try:
            has_midi = any(child.suffix.lower() in (".mid", ".midi") for child in directory.iterdir() if child.is_file())
        except OSError:
            continue
        if has_midi:
            song_dirs.append(directory)
    return sorted(song_dirs)


def track_monophony(notes: tuple[Note, ...]) -> float:
    if len(notes) < 2:
        return 1.0
    overlaps = 0
    previous_end = -math.inf
    for note in sorted(notes, key=lambda item: (item.start, item.end)):
        if note.start < previous_end - 0.03:
            overlaps += 1
        previous_end = max(previous_end, note.end)
    return max(0.0, 1.0 - overlaps / max(len(notes), 1))


def phrase_pause_score(notes: tuple[Note, ...]) -> float:
    if len(notes) < 4:
        return 0.0
    ordered = sorted(notes, key=lambda item: item.start)
    gaps = [max(0.0, b.start - a.end) for a, b in zip(ordered, ordered[1:])]
    if not gaps:
        return 0.0
    long_gaps = sum(1 for gap in gaps if gap >= 0.75)
    short_gaps = sum(1 for gap in gaps if 0.05 <= gap < 0.75)
    return min(1.0, (long_gaps * 0.18) + (short_gaps * 0.015))


def vocal_range_score(notes: tuple[Note, ...]) -> float:
    if not notes:
        return 0.0
    pitches = [note.pitch for note in notes]
    median_pitch = statistics.median(pitches)
    in_range = sum(1 for pitch in pitches if 43 <= pitch <= 84) / len(pitches)
    median_score = 1.0 - min(abs(median_pitch - 64) / 28.0, 1.0)
    return (in_range * 0.75) + (median_score * 0.25)


def select_lead_candidate(tracks: tuple[MidiTrackData, ...]) -> tuple[LeadCandidate | None, list[LeadCandidate]]:
    candidates: list[LeadCandidate] = []
    for track in tracks:
        if len(track.notes) < 8:
            continue
        source_notes = tuple(note for note in track.notes if note.channel != 9)
        if len(source_notes) < 8:
            continue
        note_sources = sorted({note.source for note in source_notes})
        source_text = " ".join(note_sources or (track.path.stem,))
        display_source = next((source for source in note_sources if has_any(source, VOCAL_HINTS)), track.path.stem)

        name_bonus = 0.0
        has_vocal_hint = has_any(source_text, VOCAL_HINTS)
        if has_vocal_hint:
            name_bonus += 0.32
        is_backing_hint = has_any(source_text, BACKING_VOCAL_HINTS)
        if is_backing_hint:
            name_bonus -= 0.23
        if has_any(source_text, NON_VOCAL_HINTS):
            name_bonus -= 0.20

        monophony = track_monophony(source_notes)
        range_fit = vocal_range_score(source_notes)
        phrasing = phrase_pause_score(source_notes)
        has_lyrics = bool(track.lyrics)
        lyric_bonus = 0.16 if has_lyrics else 0.0
        note_count_score = min(len(source_notes) / 80.0, 1.0) * 0.10
        confidence = max(
            0.0,
            min(0.98, (monophony * 0.28) + (range_fit * 0.22) + phrasing + note_count_score + lyric_bonus + name_bonus),
        )

        reasons = [
            f"monophony={monophony:.2f}",
            f"range={range_fit:.2f}",
            f"phrasing={phrasing:.2f}",
        ]
        if name_bonus > 0:
            reasons.append("name_hint")
        if lyric_bonus:
            reasons.append("lyrics")
        if name_bonus < 0:
            reasons.append("non_vocal_name_penalty")
        candidates.append(
            LeadCandidate(display_source, source_notes, confidence, ", ".join(reasons), has_vocal_hint, has_lyrics, is_backing_hint)
        )

    candidates.sort(key=lambda item: (not item.is_backing_hint, item.confidence), reverse=True)
    return (candidates[0] if candidates else None, candidates[:5])


def attach_lyrics(notes: tuple[Note, ...], lyrics: tuple[tuple[float, str], ...]) -> tuple[Note, ...]:
    if not lyrics:
        return notes
    output = list(notes)
    lyric_index = 0
    for index, note in enumerate(output):
        while lyric_index < len(lyrics) and lyrics[lyric_index][0] < note.start - 0.25:
            lyric_index += 1
        if lyric_index < len(lyrics) and abs(lyrics[lyric_index][0] - note.start) <= 0.75:
            output[index] = Note(
                start=note.start,
                duration=note.duration,
                pitch=note.pitch,
                velocity=note.velocity,
                source=note.source,
                channel=note.channel,
                track_name=note.track_name,
                lyric=lyrics[lyric_index][1],
            )
            lyric_index += 1
    return tuple(output)


def best_key(notes: Iterable[Note]) -> str:
    weights = [0.0] * 12
    for note in notes:
        if note.channel == 9:
            continue
        weights[note.pitch % 12] += max(note.duration, 0.05)
    if not any(weights):
        return "C major"

    def corr(profile: tuple[float, ...], tonic: int) -> float:
        rotated = [profile[(pc - tonic) % 12] for pc in range(12)]
        mean_w = statistics.mean(weights)
        mean_p = statistics.mean(rotated)
        numerator = sum((w - mean_w) * (p - mean_p) for w, p in zip(weights, rotated))
        denom_w = math.sqrt(sum((w - mean_w) ** 2 for w in weights))
        denom_p = math.sqrt(sum((p - mean_p) ** 2 for p in rotated))
        return numerator / (denom_w * denom_p) if denom_w and denom_p else 0.0

    scored: list[tuple[float, int, str]] = []
    for tonic in range(12):
        scored.append((corr(MAJOR_PROFILE, tonic), tonic, "major"))
        scored.append((corr(MINOR_PROFILE, tonic), tonic, "minor"))
    _, tonic, mode = max(scored, key=lambda item: item[0])
    return f"{plain_note_name(tonic)} {mode}"


def score_chord(pitch_weights: Counter[int], bass_pc: int) -> ChordGuess | None:
    if not pitch_weights:
        return None
    total = sum(pitch_weights.values())
    ranked: list[tuple[float, int, str, str, tuple[int, ...]]] = []
    present = {pc for pc, weight in pitch_weights.items() if weight / total >= 0.04}
    for root in range(12):
        for quality, suffix, intervals in CHORD_TEMPLATES:
            chord_pcs = {(root + interval) % 12 for interval in intervals}
            covered = sum(pitch_weights.get(pc, 0.0) for pc in chord_pcs) / total
            extra = sum(pitch_weights.get(pc, 0.0) for pc in present - chord_pcs) / total
            root_bonus = 0.08 if root in present else 0.0
            bass_bonus = 0.05 if bass_pc == root else 0.0
            score = covered - (extra * 0.35) + root_bonus + bass_bonus
            ranked.append((score, root, quality, suffix, intervals))
    ranked.sort(reverse=True, key=lambda item: item[0])
    best = ranked[0]
    alternatives = []
    for item in ranked[1:5]:
        if best[0] - item[0] <= 0.08:
            alternatives.append(f"{plain_note_name(item[1])}{item[3]}")
    return ChordGuess(
        name=f"{plain_note_name(best[1])}{best[3]}",
        root=best[1],
        quality=best[2],
        bass=bass_pc,
        intervals=best[4],
        confidence=max(0.25, min(0.98, best[0])),
        alternatives=tuple(alternatives),
    )


def collect_window_weights(notes: tuple[Note, ...], start: float, end: float) -> tuple[Counter[int], int | None]:
    weights: Counter[int] = Counter()
    bass_notes: list[tuple[int, float]] = []
    for note in notes:
        if note.channel == 9 or note.end <= start or note.start >= end:
            continue
        overlap = min(note.end, end) - max(note.start, start)
        if overlap <= 0.0:
            continue
        weights[note.pitch % 12] += overlap
        bass_notes.append((note.pitch, overlap))
    if not bass_notes:
        return weights, None
    bass_pitch = min((pitch for pitch, overlap in bass_notes if overlap >= 0.05), default=min(pitch for pitch, _ in bass_notes))
    return weights, bass_pitch % 12


def detect_chords(notes: tuple[Note, ...], meter: str) -> list[dict[str, object]]:
    if not notes:
        return []
    end_beat = max(note.end for note in notes)
    beats_per_bar = int(meter.split("/", 1)[0]) if "/" in meter else 4
    window = 1.0
    raw: list[tuple[float, float, ChordGuess]] = []
    current = 0.0
    while current < end_beat:
        weights, bass_pc = collect_window_weights(notes, current, current + window)
        if bass_pc is not None:
            guess = score_chord(weights, bass_pc)
            if guess and guess.confidence >= 0.38:
                raw.append((current, window, guess))
        current += window

    merged: list[tuple[float, float, ChordGuess]] = []
    for start, duration, guess in raw:
        if merged and merged[-1][2].name == guess.name and merged[-1][2].bass == guess.bass:
            prev_start, prev_duration, prev_guess = merged[-1]
            merged[-1] = (prev_start, prev_duration + duration, prev_guess)
        else:
            merged.append((start, duration, guess))

    chords: list[dict[str, object]] = []
    for index, (start, duration, guess) in enumerate(merged, start=1):
        bar, beat = bar_beat(start, beats_per_bar)
        notes_out = [midi_note_name(48 + ((guess.root + interval) % 12)) for interval in guess.intervals]
        chord: dict[str, object] = {
            "id": f"chord_{index:03d}",
            "start": round(start, 4),
            "duration": round(duration, 4),
            "bar": bar,
            "beat": beat,
            "chord": guess.name if guess.bass == guess.root else f"{guess.name}/{plain_note_name(guess.bass)}",
            "root": plain_note_name(guess.root),
            "quality": guess.quality,
            "bass": plain_note_name(guess.bass),
            "notes": notes_out,
            "confidence": round(guess.confidence, 3),
        }
        if guess.alternatives:
            chord["alternatives"] = list(guess.alternatives)
        chords.append(chord)
    return chords


def bar_beat(start: float, beats_per_bar: int) -> tuple[int, float]:
    bar = int(start // beats_per_bar) + 1
    beat = (start % beats_per_bar) + 1.0
    return bar, round(beat, 4)


def chord_ref_for_note(note: Note, chords: list[dict[str, object]]) -> str | None:
    for chord in chords:
        start = float(chord["start"])
        end = start + float(chord["duration"])
        if start <= note.start < end:
            return str(chord["id"])
    return str(chords[-1]["id"]) if chords else None


def phrase_ids(notes: tuple[Note, ...]) -> dict[int, str]:
    phrases: dict[int, str] = {}
    phrase_index = 1
    previous_end: float | None = None
    for index, note in enumerate(notes):
        if previous_end is not None and note.start - previous_end >= 1.25:
            phrase_index += 1
        phrases[index] = f"phrase_{phrase_index:03d}"
        previous_end = note.end
    return phrases


def yaml_scalar(value: object) -> str:
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "true" if value else "false"
    if isinstance(value, (int, float)):
        return str(value)
    text = str(value)
    if not text:
        return '""'
    safe = all(char.isalnum() or char in " _-./:#" for char in text)
    if safe and not text.lower() in {"null", "true", "false"}:
        return text
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def yaml_list(values: Iterable[object]) -> str:
    return "[" + ", ".join(yaml_scalar(value) for value in values) + "]"


def write_yaml(
    path: Path,
    title: str,
    tempo: int,
    meter: str,
    key: str,
    ticks_per_beat: int,
    chords: list[dict[str, object]],
    lead_notes: tuple[Note, ...],
    lead: LeadCandidate,
    candidates: list[LeadCandidate],
    chord_sources: list[str],
    warnings: list[str],
) -> None:
    beats_per_bar = int(meter.split("/", 1)[0]) if "/" in meter else 4
    phrases = phrase_ids(lead_notes)
    lines: list[str] = [
        "meta:",
        f"  title: {yaml_scalar(title)}",
        f"  tempo: {tempo}",
        f"  meter: {yaml_scalar(meter)}",
        f"  key: {yaml_scalar(key)}",
        f"  ticks_per_beat: {ticks_per_beat}",
        "  source_resolution: midi_ticks",
        "",
        "tracks:",
        "  chords:",
    ]
    if not chords:
        lines.append("    []")
    for chord in chords:
        lines.extend(
            [
                f"    - id: {chord['id']}",
                f"      start: {chord['start']}",
                f"      duration: {chord['duration']}",
                f"      bar: {chord['bar']}",
                f"      beat: {chord['beat']}",
                f"      chord: {yaml_scalar(chord['chord'])}",
                f"      root: {yaml_scalar(chord['root'])}",
                f"      quality: {yaml_scalar(chord['quality'])}",
                f"      bass: {yaml_scalar(chord['bass'])}",
                f"      notes: {yaml_list(chord['notes'])}",
                f"      confidence: {chord['confidence']}",
            ]
        )
        if "alternatives" in chord:
            lines.append(f"      alternatives: {yaml_list(chord['alternatives'])}")

    lines.extend(["", "  lead_vocal:"])
    for index, note in enumerate(lead_notes, start=1):
        bar, beat = bar_beat(note.start, beats_per_bar)
        lines.extend(
            [
                f"    - id: lead_{index:03d}",
                f"      start: {round(note.start, 4)}",
                f"      duration: {round(note.duration, 4)}",
                f"      bar: {bar}",
                f"      beat: {beat}",
                f"      pitch: {midi_note_name(note.pitch)}",
                f"      midi_note: {note.pitch}",
                f"      velocity: {note.velocity}",
                f"      syllable: {yaml_scalar(note.lyric)}",
                f"      phrase_id: {phrases[index - 1]}",
                f"      chord_ref: {yaml_scalar(chord_ref_for_note(note, chords))}",
            ]
        )

    chord_confidence = round(sum(float(chord["confidence"]) for chord in chords) / len(chords), 3) if chords else 0.0
    lines.extend(
        [
            "",
            "analysis:",
            "  lead_detection:",
            f"    selected_track: {yaml_scalar(lead.source)}",
            f"    confidence: {round(lead.confidence, 3)}",
            f"    reason: {yaml_scalar(lead.reason)}",
            "    candidates:",
        ]
    )
    for candidate in candidates:
        lines.extend(
            [
                f"      - track_name: {yaml_scalar(candidate.source)}",
                f"        confidence: {round(candidate.confidence, 3)}",
            ]
        )
    lines.extend(
        [
            "",
            "  chord_detection:",
            f"    source_tracks: {yaml_list(chord_sources)}",
            f"    confidence: {chord_confidence}",
            "    method: beat_window_pitch_class_template_matching",
            "",
            f"  warnings: {yaml_list(warnings)}",
        ]
    )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def output_path_for(song_dir: Path, midi_root: Path, output_root: Path) -> Path:
    relative = song_dir.relative_to(midi_root)
    return output_root / relative.parent / f"{song_dir.name}.yaml"


def process_song(
    song_dir: Path,
    midi_root: Path,
    output_root: Path,
    min_lead_confidence: float,
    allow_heuristic_lead: bool,
    allow_empty_chords: bool,
    overwrite: bool,
) -> SongResult:
    destination = output_path_for(song_dir, midi_root, output_root)
    if destination.exists() and not overwrite:
        return SongResult(song_dir, destination, False, "exists")

    midi_paths = sorted(path for path in song_dir.iterdir() if path.is_file() and path.suffix.lower() in (".mid", ".midi"))
    if not midi_paths:
        return SongResult(song_dir, destination, False, "no_midi")

    try:
        tracks = tuple(read_midi_file(path) for path in midi_paths)
    except Exception as exc:  # noqa: BLE001 - batch conversion should continue past corrupt MIDI files.
        return SongResult(song_dir, destination, False, f"parse_error: {exc}")

    lead, candidates = select_lead_candidate(tracks)
    if lead is None:
        return SongResult(song_dir, destination, False, "no_lead_candidate")
    if lead.is_backing_hint:
        return SongResult(song_dir, destination, False, "selected_backing_vocal", lead.confidence)
    if not allow_heuristic_lead and not lead.has_vocal_hint and not lead.has_lyrics:
        return SongResult(song_dir, destination, False, "no_vocal_hint", lead.confidence)
    if lead.confidence < min_lead_confidence:
        return SongResult(song_dir, destination, False, f"low_lead_confidence: {lead.confidence:.3f}", lead.confidence)

    lead_sources = {note.source.split("::", 1)[0] for note in lead.notes}
    lyrics = next((track.lyrics for track in tracks if track.path.stem in lead_sources and track.lyrics), tuple())
    lead_notes = attach_lyrics(lead.notes, lyrics)
    chord_notes = tuple(
        note
        for track in tracks
        for note in track.notes
        if track.path.stem not in lead_sources and note.channel != 9 and not has_any(track.path.stem, ("drum", "bateria", "perc"))
    )

    meter = next((track.meter for track in tracks if track.meter), "4/4")
    ticks = Counter(track.ticks_per_beat for track in tracks).most_common(1)[0][0]
    tempo = bpm_from_tempo(next((track.tempo for track in tracks if track.tempo), None))
    all_notes = tuple(note for track in tracks for note in track.notes)
    key = next((track.key for track in tracks if track.key), None) or best_key(all_notes)
    chords = detect_chords(chord_notes, meter)
    chord_sources = sorted({note.source.split("::", 1)[0] for note in chord_notes})
    warnings: list[str] = []
    if lead.confidence < 0.68:
        warnings.append("low_confidence_lead_detection")
    if not chords:
        if not allow_empty_chords:
            return SongResult(song_dir, destination, False, "no_chords_detected", lead.confidence)
        warnings.append("no_chords_detected")
    if not lyrics:
        warnings.append("no_lyrics_events")

    write_yaml(
        destination,
        song_dir.name,
        tempo,
        meter,
        key,
        ticks,
        chords,
        lead_notes,
        lead,
        candidates,
        chord_sources,
        warnings,
    )
    return SongResult(song_dir, destination, True, lead_confidence=lead.confidence, chord_count=len(chords), lead_note_count=len(lead_notes))


def drain_one(pending: set[Future[SongResult]]) -> list[SongResult]:
    completed, _ = wait(pending, return_when=FIRST_COMPLETED)
    results: list[SongResult] = []
    for future in completed:
        pending.remove(future)
        results.append(future.result())
    return results


def main() -> None:
    args = parse_args()
    midi_root = args.midi_root.expanduser().resolve()
    output_root = (args.output_root or midi_root.parent / "dsl").expanduser().resolve()
    song_dirs = group_song_dirs(midi_root)
    if args.limit:
        song_dirs = song_dirs[: args.limit]

    output_root.mkdir(parents=True, exist_ok=True)
    pending: set[Future[SongResult]] = set()
    results: list[SongResult] = []
    max_pending = max(args.workers * 3, 1)
    print(f"Discovered {len(song_dirs)} song folders under {midi_root}")
    print(f"Writing DSL files under {output_root}")

    with ThreadPoolExecutor(max_workers=max(args.workers, 1)) as executor:
        for song_dir in song_dirs:
            pending.add(
                executor.submit(
                    process_song,
                    song_dir,
                    midi_root,
                    output_root,
                    args.min_lead_confidence,
                    args.allow_heuristic_lead,
                    args.allow_empty_chords,
                    args.overwrite,
                )
            )
            if len(pending) >= max_pending:
                results.extend(drain_one(pending))
                report_progress(results)
        while pending:
            results.extend(drain_one(pending))
            report_progress(results)

    written = sum(1 for result in results if result.written)
    skipped = len(results) - written
    print(f"Complete: wrote {written} DSL files, skipped {skipped} folders")
    reasons = Counter(result.reason for result in results if not result.written)
    for reason, count in reasons.most_common(10):
        print(f"  skipped {count}: {reason}")


def report_progress(results: list[SongResult]) -> None:
    count = len(results)
    if count % 500 != 0:
        return
    written = sum(1 for result in results if result.written)
    print(f"Processed {count} folders, wrote {written} DSL files", flush=True)


if __name__ == "__main__":
    main()
