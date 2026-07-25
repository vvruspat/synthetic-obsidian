#!/usr/bin/env python3
"""Generate backing-vocal arrangement tracks inside Synthetic Obsidian DSL YAML."""

from __future__ import annotations

import argparse
import math
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable


NOTE_TO_PC = {"C": 0, "C#": 1, "DB": 1, "D": 2, "D#": 3, "EB": 3, "E": 4, "F": 5, "F#": 6, "GB": 6, "G": 7, "G#": 8, "AB": 8, "A": 9, "A#": 10, "BB": 10, "B": 11}
PC_TO_NOTE = ("C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
MAJOR_SCALE = (0, 2, 4, 5, 7, 9, 11)
MINOR_SCALE = (0, 2, 3, 5, 7, 8, 10)

VOICE_RANGES = {
    "general": (48, 79),
    "soprano": (60, 81),
    "alto": (55, 74),
    "tenor": (48, 67),
    "bass": (40, 60),
}

STRATEGIES = (
    ("UNISON", "Unison Double", "Unison double following the lead melody.", (("unison", "general"),)),
    ("OCT_UP", "Octave Above", "Octave doubling above the lead.", (("oct_up", "soprano"),)),
    ("OCT_DOWN", "Octave Below", "Octave doubling below the lead.", (("oct_down", "tenor"),)),
    ("THIRD_UP", "Third Above", "Chord-aware upper third harmony.", (("third_up", "alto"),)),
    ("THIRD_DOWN", "Third Below", "Chord-aware lower third harmony.", (("third_down", "tenor"),)),
    ("SIXTH_UP", "Sixth Above", "Upper sixth harmony with range correction.", (("sixth_up", "soprano"),)),
    ("SIXTH_DOWN", "Sixth Below", "Lower sixth harmony with range correction.", (("sixth_down", "tenor"),)),
    ("FIFTH_UP", "Fifth Above", "Upper fifth harmony, softened toward chord tones when needed.", (("fifth_up", "alto"),)),
    ("FIFTH_DOWN", "Fifth Below", "Lower fifth harmony, softened toward chord tones when needed.", (("fifth_down", "tenor"),)),
    ("FOURTH_UP", "Fourth Above", "Upper fourth harmony used as a color interval.", (("fourth_up", "alto"),)),
    ("FOURTH_DOWN", "Fourth Below", "Lower fourth harmony used as a color interval.", (("fourth_down", "tenor"),)),
    ("DRONE_ROOT", "Drone Root", "Phrase-level sustained chord root.", (("drone_root", "alto"),)),
    ("DRONE_FIFTH", "Drone Fifth", "Phrase-level sustained chord fifth.", (("drone_fifth", "alto"),)),
    ("DRONE_THIRD", "Drone Third", "Phrase-level sustained chord third.", (("drone_third", "alto"),)),
    ("PEDAL_ROOT", "Pedal Tone Root", "Long pedal on the global tonic or chord root.", (("pedal_root", "tenor"),)),
    ("PEDAL_FIFTH", "Pedal Tone Fifth", "Long pedal on the global fifth.", (("pedal_fifth", "tenor"),)),
    ("CONTRARY", "Contrary Motion Harmony", "Harmony shaped to move against the lead contour.", (("contrary", "alto"),)),
    ("OBLIQUE", "Oblique Motion Harmony", "Harmony that sustains common tones while lead moves.", (("oblique", "alto"),)),
    ("PAR3", "Parallel Thirds", "Mostly parallel thirds with chord-tone correction.", (("parallel_thirds", "alto"),)),
    ("PAR6", "Parallel Sixths", "Mostly parallel sixths with chord-tone correction.", (("parallel_sixths", "soprano"),)),
    ("CHOIR_SOP", "Choir Soprano", "SATB soprano line.", (("choir_soprano", "soprano"),)),
    ("CHOIR_ALT", "Choir Alto", "SATB alto line.", (("choir_alto", "alto"),)),
    ("CHOIR_TEN", "Choir Tenor", "SATB tenor line.", (("choir_tenor", "tenor"),)),
    ("CHOIR_BASS", "Choir Bass", "SATB bass line.", (("choir_bass", "bass"),)),
    ("HARMONY_2P", "Two-Part Harmony", "Natural two-part backing around the lead.", (("third_up", "alto"), ("sixth_down", "tenor"))),
    ("HARMONY_3P", "Three-Part Harmony", "Three-part chord-aware backing.", (("third_up", "alto"), ("sixth_down", "tenor"), ("drone_root", "bass"))),
    ("HARMONY_4P", "Four-Part Harmony", "SATB-style four-part backing.", (("choir_soprano", "soprano"), ("choir_alto", "alto"), ("choir_tenor", "tenor"), ("choir_bass", "bass"))),
    ("STYLE_POP", "Pop Harmony", "Modern pop thirds/sixths with restrained tension.", (("third_up", "alto"), ("third_down", "tenor"), ("oct_up_sparse", "soprano"))),
    ("STYLE_FOLK", "Folk Harmony", "Open folk voicing with fifths and drones.", (("fifth_up", "alto"), ("drone_root", "tenor"))),
    ("STYLE_GOSPEL", "Gospel Harmony", "Dense gospel-inspired chord tones and color tones.", (("gospel_top", "soprano"), ("choir_alto", "alto"), ("choir_tenor", "tenor"), ("choir_bass", "bass"))),
    ("STYLE_CLASSICAL", "Classical Choral Harmony", "Conservative classical SATB voice leading.", (("classical_soprano", "soprano"), ("choir_alto", "alto"), ("choir_tenor", "tenor"), ("choir_bass", "bass"))),
    ("STYLE_BSHOP", "Barbershop Harmony", "Close-position barbershop-inspired harmony.", (("barbershop_tenor", "soprano"), ("barbershop_lead_support", "alto"), ("barbershop_bari", "tenor"), ("barbershop_bass", "bass"))),
    ("SUSP", "Suspension Harmony", "Prepared suspensions resolving into chord tones.", (("suspension", "alto"),)),
    ("PASSING", "Passing Tone Harmony", "Chord-tone harmony with passing motion between anchors.", (("passing", "alto"),)),
    ("TENSION_RES", "Tension-Resolution Harmony", "Intentional tension resolving to stable chord tones.", (("tension_resolution", "alto"),)),
)


@dataclass(frozen=True)
class Note:
    id: str
    start: float
    duration: float
    bar: int
    beat: float
    pitch: str
    midi_note: int
    velocity: int
    syllable: str | None
    phrase_id: str
    chord_ref: str

    @property
    def end(self) -> float:
        return self.start + self.duration


@dataclass(frozen=True)
class Chord:
    id: str
    start: float
    duration: float
    bar: int
    beat: float
    chord: str
    root: str
    quality: str
    bass: str
    notes: tuple[str, ...]

    @property
    def end(self) -> float:
        return self.start + self.duration


@dataclass(frozen=True)
class Song:
    meta: dict[str, object]
    chords: tuple[Chord, ...]
    lead: tuple[Note, ...]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--in-place", action="store_true")
    return parser.parse_args()


def split_pair(text: str) -> tuple[str, str]:
    index = text.index(":")
    return text[:index].strip(), text[index + 1 :].strip()


def parse_scalar(value: str) -> object:
    value = value.strip()
    if value == "null":
        return None
    if value == "":
        return ""
    if value.startswith("[") and value.endswith("]"):
        return tuple(parse_scalar(item.strip()) for item in split_list(value[1:-1]))
    if (value.startswith('"') and value.endswith('"')) or (value.startswith("'") and value.endswith("'")):
        return value[1:-1].replace('\\"', '"').replace("\\\\", "\\")
    try:
        if "." in value:
            return float(value)
        return int(value)
    except ValueError:
        return value


def split_list(value: str) -> list[str]:
    items: list[str] = []
    token = ""
    quote = ""
    for char in value:
        if quote:
            token += char
            if char == quote:
                quote = ""
        elif char in ("'", '"'):
            quote = char
            token += char
        elif char == ",":
            items.append(token)
            token = ""
        else:
            token += char
    if token.strip():
        items.append(token)
    return items


def parse_song(text: str) -> Song:
    meta: dict[str, object] = {}
    chords: list[dict[str, object]] = []
    lead: list[dict[str, object]] = []
    section = ""
    subsection = ""
    current: dict[str, object] | None = None

    for raw_line in text.splitlines():
        if not raw_line.strip():
            continue
        indent = len(raw_line) - len(raw_line.lstrip(" "))
        line = raw_line.strip()
        if indent == 0 and line.endswith(":"):
            section = line[:-1]
            subsection = ""
            current = None
            continue
        if section == "meta" and indent == 2:
            key, value = split_pair(line)
            meta[key] = parse_scalar(value)
            continue
        if section == "tracks" and indent == 2 and line.endswith(":"):
            subsection = line[:-1]
            current = None
            continue
        if section == "tracks" and indent == 4 and line.startswith("- "):
            current = {}
            target = chords if subsection == "chords" else lead if subsection == "lead_vocal" else None
            if target is not None:
                target.append(current)
            rest = line[2:]
            if ":" in rest and current is not None:
                key, value = split_pair(rest)
                current[key] = parse_scalar(value)
            continue
        if section == "tracks" and current is not None and indent >= 6 and ":" in line:
            key, value = split_pair(line)
            current[key] = parse_scalar(value)

    return Song(
        meta=meta,
        chords=tuple(to_chord(item) for item in chords),
        lead=tuple(to_note(item) for item in lead),
    )


def to_note(item: dict[str, object]) -> Note:
    return Note(
        id=str(item["id"]),
        start=float(item["start"]),
        duration=float(item["duration"]),
        bar=int(item["bar"]),
        beat=float(item["beat"]),
        pitch=str(item["pitch"]),
        midi_note=int(item["midi_note"]),
        velocity=int(item["velocity"]),
        syllable=item.get("syllable") if isinstance(item.get("syllable"), str) else None,
        phrase_id=str(item["phrase_id"]),
        chord_ref=str(item["chord_ref"]),
    )


def to_chord(item: dict[str, object]) -> Chord:
    notes = item.get("notes")
    return Chord(
        id=str(item["id"]),
        start=float(item["start"]),
        duration=float(item["duration"]),
        bar=int(item["bar"]),
        beat=float(item["beat"]),
        chord=str(item["chord"]),
        root=str(item["root"]),
        quality=str(item["quality"]),
        bass=str(item["bass"]),
        notes=tuple(str(note) for note in notes) if isinstance(notes, tuple) else tuple(),
    )


def note_name(midi: int) -> str:
    return f"{PC_TO_NOTE[midi % 12]}{midi // 12 - 1}"


def note_pc(note: str) -> int:
    name = note[:-1].upper()
    return NOTE_TO_PC[name]


def note_to_midi(note: str) -> int:
    octave = int(note[-1])
    return (octave + 1) * 12 + note_pc(note)


def root_pc(chord: Chord) -> int:
    return NOTE_TO_PC[chord.root.upper()]


def chord_pcs(chord: Chord) -> set[int]:
    pcs = {note_to_midi(note) % 12 for note in chord.notes}
    if pcs:
        return pcs
    root = root_pc(chord)
    if "minor" in chord.quality:
        return {root, (root + 3) % 12, (root + 7) % 12}
    if "sus4" in chord.quality:
        return {root, (root + 5) % 12, (root + 7) % 12}
    if "sus2" in chord.quality:
        return {root, (root + 2) % 12, (root + 7) % 12}
    return {root, (root + 4) % 12, (root + 7) % 12}


def key_scale(meta: dict[str, object]) -> set[int]:
    key = str(meta.get("key", "C major"))
    parts = key.split()
    tonic = NOTE_TO_PC.get(parts[0].upper(), 0)
    mode = parts[1].lower() if len(parts) > 1 else "major"
    scale = MINOR_SCALE if "minor" in mode else MAJOR_SCALE
    return {(tonic + step) % 12 for step in scale}


def chord_at(song: Song, ref: str, start: float) -> Chord:
    for chord in song.chords:
        if chord.id == ref:
            return chord
    for chord in song.chords:
        if chord.start <= start < chord.end:
            return chord
    return song.chords[-1]


def choose_pitch(
    desired: int,
    chord: Chord,
    scale: set[int],
    voice_range: tuple[int, int],
    previous: int | None = None,
    prefer_chord: bool = True,
    avoid_unison_with: int | None = None,
) -> int:
    low, high = voice_range
    candidates = range(max(24, low - 12), min(96, high + 12) + 1)
    chord_set = chord_pcs(chord)
    best = desired
    best_score = math.inf
    for pitch in candidates:
        pc = pitch % 12
        score = abs(pitch - desired)
        if pitch < low or pitch > high:
            score += 8 + min(abs(pitch - low), abs(pitch - high))
        if prefer_chord and pc not in chord_set:
            score += 3.5
        elif pc in chord_set:
            score -= 1.0
        if pc not in scale and pc not in chord_set:
            score += 2.0
        if previous is not None:
            leap = abs(pitch - previous)
            if leap > 7:
                score += (leap - 7) * 0.7
            score += abs(pitch - previous) * 0.08
        if avoid_unison_with is not None and pitch == avoid_unison_with:
            score += 5.0
        if score < best_score:
            best_score = score
            best = pitch
    return int(best)


def transpose_to_range(pitch: int, voice_range: tuple[int, int]) -> int:
    low, high = voice_range
    while pitch < low:
        pitch += 12
    while pitch > high:
        pitch -= 12
    return max(low, min(high, pitch))


def target_for_strategy(strategy: str, note: Note, chord: Chord, scale: set[int], previous: int | None, index: int, lead: tuple[Note, ...]) -> int:
    intervals = {
        "unison": 0,
        "oct_up": 12,
        "oct_down": -12,
        "third_up": 4,
        "third_down": -3,
        "sixth_up": 9,
        "sixth_down": -8,
        "fifth_up": 7,
        "fifth_down": -7,
        "fourth_up": 5,
        "fourth_down": -5,
        "parallel_thirds": 4,
        "parallel_sixths": 9,
        "oct_up_sparse": 12,
    }
    if strategy in intervals:
        return note.midi_note + intervals[strategy]
    if strategy in ("choir_soprano", "classical_soprano"):
        return note.midi_note + 7
    if strategy in ("choir_alto", "barbershop_lead_support"):
        return note.midi_note + 3
    if strategy in ("choir_tenor", "barbershop_bari"):
        return note.midi_note - 5
    if strategy in ("choir_bass", "barbershop_bass"):
        return nearest_pc(root_pc(chord), 48, previous)
    if strategy == "barbershop_tenor":
        return note.midi_note + 10
    if strategy == "gospel_top":
        return note.midi_note + (10 if index % 4 == 2 else 7)
    if strategy == "contrary":
        if previous is None or index == 0:
            return note.midi_note + 4
        lead_motion = note.midi_note - lead[index - 1].midi_note
        return previous - clamp_int(lead_motion, -5, 5)
    if strategy == "oblique":
        if previous is not None and previous % 12 in chord_pcs(chord):
            return previous
        return nearest_pc(root_pc(chord), note.midi_note + 3, previous)
    if strategy == "suspension":
        if previous is not None and index % 3 == 0:
            return previous
        return note.midi_note + (5 if index % 3 == 1 else 4)
    if strategy == "passing":
        if previous is not None and index + 1 < len(lead):
            next_target = lead[index + 1].midi_note + 4
            if abs(next_target - previous) <= 5:
                return round((previous + next_target) / 2)
        return note.midi_note + 4
    if strategy == "tension_resolution":
        return note.midi_note + (10 if index % 4 in (1, 2) else 4)
    return note.midi_note + 4


def nearest_pc(pc: int, around: int, previous: int | None = None) -> int:
    center = previous if previous is not None else around
    candidates = [pitch for pitch in range(center - 18, center + 19) if pitch % 12 == pc]
    return min(candidates, key=lambda pitch: abs(pitch - around)) if candidates else around


def clamp_int(value: int, low: int, high: int) -> int:
    return min(max(value, low), high)


def generate_melodic_part(song: Song, strategy: str, role: str) -> list[dict[str, object]]:
    scale = key_scale(song.meta)
    voice_range = VOICE_RANGES[role]
    output: list[dict[str, object]] = []
    previous: int | None = None
    for index, note in enumerate(song.lead):
        if strategy == "oct_up_sparse" and index % 3 == 1:
            continue
        chord = chord_at(song, note.chord_ref, note.start)
        desired = target_for_strategy(strategy, note, chord, scale, previous, index, song.lead)
        prefer_chord = strategy not in ("unison", "oct_up", "oct_down")
        pitch = choose_pitch(desired, chord, scale, voice_range, previous, prefer_chord, note.midi_note)
        if strategy in ("unison", "oct_up", "oct_down"):
            pitch = transpose_to_range(desired, voice_range)
        output.append(note_dict(index + 1, note, pitch, strategy))
        previous = pitch
    return output


def generate_sustained_part(song: Song, strategy: str, role: str) -> list[dict[str, object]]:
    voice_range = VOICE_RANGES[role]
    groups = phrase_groups(song.lead)
    output: list[dict[str, object]] = []
    previous: int | None = None
    for index, phrase_notes in enumerate(groups, start=1):
        first = phrase_notes[0]
        last = phrase_notes[-1]
        chord = chord_at(song, first.chord_ref, first.start)
        pc = sustained_pc(song, strategy, chord)
        target = nearest_pc(pc, first.midi_note - 3, previous)
        pitch = transpose_to_range(target, voice_range)
        duration = max(0.25, last.end - first.start)
        output.append(
            {
                "id": f"{strategy}_{index:03d}",
                "start": round(first.start, 4),
                "duration": round(duration, 4),
                "bar": first.bar,
                "beat": first.beat,
                "pitch": note_name(pitch),
                "midi_note": pitch,
                "velocity": max(45, min(84, int(first.velocity * 0.72))),
                "syllable": None,
                "phrase_id": first.phrase_id,
                "chord_ref": first.chord_ref,
                "source_lead_ref": first.id,
            }
        )
        previous = pitch
    return output


def generate_rhythmic_drone_part(song: Song, strategy: str, role: str) -> list[dict[str, object]]:
    voice_range = VOICE_RANGES[role]
    output: list[dict[str, object]] = []
    previous: int | None = None
    for index, note in enumerate(song.lead, start=1):
        chord = chord_at(song, note.chord_ref, note.start)
        pc = sustained_pc(song, strategy, chord)
        target = nearest_pc(pc, note.midi_note - 3, previous)
        pitch = transpose_to_range(target, voice_range)
        drone_note = note_dict(index, note, pitch, strategy)
        drone_note["velocity"] = max(42, min(84, int(note.velocity * 0.7)))
        output.append(drone_note)
        previous = pitch
    return output


def sustained_pc(song: Song, strategy: str, chord: Chord) -> int:
    root = root_pc(chord)
    if strategy in ("drone_fifth", "pedal_fifth"):
        return (root + 7) % 12
    if strategy == "drone_third":
        if "minor" in chord.quality:
            return (root + 3) % 12
        return (root + 4) % 12
    if strategy == "pedal_root":
        return NOTE_TO_PC.get(str(song.meta.get("key", "C")).split()[0].upper(), root)
    return root


def phrase_groups(notes: tuple[Note, ...]) -> list[list[Note]]:
    groups: list[list[Note]] = []
    for note in notes:
        if not groups or groups[-1][-1].phrase_id != note.phrase_id:
            groups.append([note])
        else:
            groups[-1].append(note)
    return groups


def note_dict(index: int, source: Note, pitch: int, prefix: str) -> dict[str, object]:
    return {
        "id": f"{prefix}_{index:03d}",
        "start": round(source.start, 4),
        "duration": round(source.duration, 4),
        "bar": source.bar,
        "beat": source.beat,
        "pitch": note_name(pitch),
        "midi_note": pitch,
        "velocity": max(42, min(90, int(source.velocity * 0.78))),
        "syllable": source.syllable,
        "phrase_id": source.phrase_id,
        "chord_ref": source.chord_ref,
        "source_lead_ref": source.id,
    }


def generate_backing(song: Song) -> list[dict[str, object]]:
    tracks: list[dict[str, object]] = []
    for track_id, name, description, parts_spec in STRATEGIES:
        parts = []
        for part_index, (strategy, role) in enumerate(parts_spec, start=1):
            if strategy.startswith("drone_"):
                notes = generate_rhythmic_drone_part(song, strategy, role)
            elif strategy.startswith("pedal_"):
                notes = generate_sustained_part(song, strategy, role)
            else:
                notes = generate_melodic_part(song, strategy, role)
            parts.append(
                {
                    "id": f"{track_id.lower()}_part_{part_index:02d}",
                    "role": role,
                    "strategy": strategy,
                    "range": f"{note_name(VOICE_RANGES[role][0])}-{note_name(VOICE_RANGES[role][1])}",
                    "notes": notes,
                }
            )
        tracks.append(
            {
                "id": track_id,
                "name": name,
                "description": description,
                "confidence": 0.74 if len(parts) == 1 else 0.7,
                "parts": parts,
            }
        )
    return tracks


def remove_existing_backing(text: str) -> str:
    lines = text.splitlines()
    output: list[str] = []
    skipping = False
    for line in lines:
        if line.startswith("  backing_vocals:"):
            skipping = True
            continue
        if skipping and (line.startswith("analysis:") or (line.startswith("  ") and not line.startswith("    "))):
            skipping = False
        if not skipping:
            output.append(line)
    return "\n".join(output).rstrip() + "\n"


def render_backing_yaml(tracks: list[dict[str, object]]) -> str:
    lines = ["  backing_vocals:"]
    for track in tracks:
        lines.extend(
            [
                f"    - id: {track['id']}",
                f"      name: {yaml_scalar(track['name'])}",
                f"      description: {yaml_scalar(track['description'])}",
                f"      confidence: {track['confidence']}",
                "      parts:",
            ]
        )
        for part in track["parts"]:
            lines.extend(
                [
                    f"        - id: {part['id']}",
                    f"          role: {part['role']}",
                    f"          strategy: {part['strategy']}",
                    f"          range: {part['range']}",
                    "          notes:",
                ]
            )
            for note in part["notes"]:
                lines.extend(
                    [
                        f"            - id: {note['id']}",
                        f"              start: {note['start']}",
                        f"              duration: {note['duration']}",
                        f"              bar: {note['bar']}",
                        f"              beat: {note['beat']}",
                        f"              pitch: {note['pitch']}",
                        f"              midi_note: {note['midi_note']}",
                        f"              velocity: {note['velocity']}",
                        f"              syllable: {yaml_scalar(note['syllable'])}",
                        f"              phrase_id: {note['phrase_id']}",
                        f"              chord_ref: {note['chord_ref']}",
                        f"              source_lead_ref: {note['source_lead_ref']}",
                    ]
                )
    return "\n".join(lines) + "\n"


def insert_backing(text: str, backing_yaml: str) -> str:
    cleaned = remove_existing_backing(text)
    marker = "\nanalysis:\n"
    if marker not in cleaned:
        return cleaned.rstrip() + "\n" + backing_yaml
    before, after = cleaned.split(marker, 1)
    return before.rstrip() + "\n\n" + backing_yaml + "\nanalysis:\n" + after


def yaml_scalar(value: object) -> str:
    if value is None:
        return "null"
    if isinstance(value, (int, float)):
        return str(value)
    text = str(value)
    if not text:
        return '""'
    safe = all(char.isalnum() or char in " _-./:#" for char in text)
    if safe and text.lower() not in {"null", "true", "false"}:
        return text
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main() -> None:
    args = parse_args()
    source = args.input.expanduser().resolve()
    destination = source if args.in_place else (args.output.expanduser().resolve() if args.output else source.with_suffix(".backing.yaml"))
    text = source.read_text(encoding="utf-8")
    song = parse_song(text)
    if not song.chords or not song.lead:
        raise SystemExit(f"{source} does not contain chords and lead_vocal tracks")
    backing = generate_backing(song)
    destination.write_text(insert_backing(text, render_backing_yaml(backing)), encoding="utf-8")
    note_count = sum(len(part["notes"]) for track in backing for part in track["parts"])
    print(f"Wrote {destination}")
    print(f"Generated {len(backing)} backing tracks, {note_count} notes across parts")


if __name__ == "__main__":
    main()
