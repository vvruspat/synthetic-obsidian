#!/usr/bin/env python3
"""Generate backing-vocal MIDI notes for the Vocal Annotation Tool.

This helper is intentionally offline-only. The JUCE app invokes it from a
background thread and passes a JSON snapshot containing musical context and
lead-vocal notes in seconds. The script converts that context to the YAML DSL
contract expected by the local Qwen/MLX LoRA model, validates the model output,
then returns backing notes in seconds for display/export.
"""

from __future__ import annotations

import argparse
import json
import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable

import yaml
from mlx_lm import generate as mlx_generate
from mlx_lm import load as mlx_load
from mlx_lm.sample_utils import make_sampler


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MODEL = "Qwen/Qwen3-0.6B"
DEFAULT_ADAPTER = (
    Path.home()
    / ".synthetic_obsidian"
    / "models"
    / "backing_vocals"
    / "qwen3_0_6b_stage2_yaml_windows_full_lora"
)
DEFAULT_PYTHON = REPO_ROOT / ".venv-llm" / "bin" / "python"
SYSTEM_PROMPT = (
    "You generate Synthetic Obsidian backing-vocal YAML DSL. "
    "Return only valid YAML containing tracks.backing_vocals. "
    "Do not include markdown, analysis, chords, lead_vocal, or explanations."
)

STYLE_DEFS: tuple[dict[str, str], ...] = (
    {"id": "UNISON", "name": "Unison Double", "description": "Unison double following the lead melody."},
    {"id": "OCT_UP", "name": "Octave Above", "description": "Octave doubling above the lead."},
    {"id": "OCT_DOWN", "name": "Octave Below", "description": "Octave doubling below the lead."},
    {"id": "THIRD_UP", "name": "Third Above", "description": "Chord-aware upper third harmony."},
    {"id": "THIRD_DOWN", "name": "Third Below", "description": "Chord-aware lower third harmony."},
    {"id": "SIXTH_UP", "name": "Sixth Above", "description": "Upper sixth harmony with range correction."},
    {"id": "SIXTH_DOWN", "name": "Sixth Below", "description": "Lower sixth harmony with range correction."},
    {"id": "FIFTH_UP", "name": "Fifth Above", "description": "Upper fifth harmony, softened toward chord tones when needed."},
    {"id": "FIFTH_DOWN", "name": "Fifth Below", "description": "Lower fifth harmony, softened toward chord tones when needed."},
    {"id": "FOURTH_UP", "name": "Fourth Above", "description": "Upper fourth harmony used as a color interval."},
    {"id": "FOURTH_DOWN", "name": "Fourth Below", "description": "Lower fourth harmony used as a color interval."},
    {"id": "DRONE_ROOT", "name": "Drone Root", "description": "Phrase-level sustained chord root."},
    {"id": "DRONE_FIFTH", "name": "Drone Fifth", "description": "Phrase-level sustained chord fifth."},
    {"id": "DRONE_THIRD", "name": "Drone Third", "description": "Phrase-level sustained chord third."},
    {"id": "PEDAL_ROOT", "name": "Pedal Tone Root", "description": "Long pedal on the global tonic or chord root."},
    {"id": "PEDAL_FIFTH", "name": "Pedal Tone Fifth", "description": "Long pedal on the global fifth."},
    {"id": "CONTRARY", "name": "Contrary Motion Harmony", "description": "Harmony shaped to move against the lead contour."},
    {"id": "OBLIQUE", "name": "Oblique Motion Harmony", "description": "Harmony that sustains common tones while lead moves."},
    {"id": "PAR3", "name": "Parallel Thirds", "description": "Mostly parallel thirds with chord-tone correction."},
    {"id": "PAR6", "name": "Parallel Sixths", "description": "Mostly parallel sixths with chord-tone correction."},
    {"id": "CHOIR_SOP", "name": "Choir Soprano", "description": "SATB soprano line."},
    {"id": "CHOIR_ALT", "name": "Choir Alto", "description": "SATB alto line."},
    {"id": "CHOIR_TEN", "name": "Choir Tenor", "description": "SATB tenor line."},
    {"id": "CHOIR_BASS", "name": "Choir Bass", "description": "SATB bass line."},
    {"id": "HARMONY_2P", "name": "Two-Part Harmony", "description": "Natural two-part backing around the lead."},
    {"id": "HARMONY_3P", "name": "Three-Part Harmony", "description": "Three-part chord-aware backing."},
    {"id": "HARMONY_4P", "name": "Four-Part Harmony", "description": "SATB-style four-part backing."},
    {"id": "STYLE_POP", "name": "Pop Harmony", "description": "Modern pop thirds/sixths with restrained tension."},
    {"id": "STYLE_FOLK", "name": "Folk Harmony", "description": "Open folk voicing with fifths and drones."},
    {"id": "STYLE_GOSPEL", "name": "Gospel Harmony", "description": "Dense gospel-inspired chord tones and color tones."},
    {"id": "STYLE_CLASSICAL", "name": "Classical Choral Harmony", "description": "Conservative classical SATB voice leading."},
    {"id": "STYLE_BSHOP", "name": "Barbershop Harmony", "description": "Close-position barbershop-inspired harmony."},
    {"id": "SUSP", "name": "Suspension Harmony", "description": "Prepared suspensions resolving into chord tones."},
    {"id": "PASSING", "name": "Passing Tone Harmony", "description": "Chord-tone harmony with passing motion between anchors."},
    {"id": "TENSION_RES", "name": "Tension-Resolution Harmony", "description": "Intentional tension resolving to stable chord tones."},
    {"id": "DYNAMIC_CP", "name": "Dynamic Counterpoint", "description": "Phrase-aware harmony alternating parallel, contrary, and oblique motion."},
)

NOTE_TO_PC = {"C": 0, "C#": 1, "Db": 1, "D": 2, "D#": 3, "Eb": 3, "E": 4, "F": 5, "F#": 6, "Gb": 6, "G": 7, "G#": 8, "Ab": 8, "A": 9, "A#": 10, "Bb": 10, "B": 11}
PC_TO_NOTE = ("C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B")


class IndentedSafeDumper(yaml.SafeDumper):
    def increase_indent(self, flow: bool = False, indentless: bool = False) -> None:  # type: ignore[override]
        return super().increase_indent(flow, False)


@dataclass(frozen=True)
class BeatPoint:
    time: float
    beat: float
    bpm: float


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("input_json", type=Path)
    parser.add_argument("--model", default=DEFAULT_MODEL)
    parser.add_argument("--adapter-path", type=Path, default=DEFAULT_ADAPTER)
    parser.add_argument("--python", type=Path, default=DEFAULT_PYTHON)
    parser.add_argument("--max-lead-notes", type=int, default=48)
    parser.add_argument("--max-gap-beats", type=float, default=2.0)
    parser.add_argument("--chord-padding-beats", type=float, default=2.0)
    parser.add_argument("--max-tokens", type=int, default=1536)
    parser.add_argument("--temperature", type=float, default=0.0)
    parser.add_argument("--top-p", type=float, default=1.0)
    parser.add_argument("--debug-dir", type=Path)
    parser.add_argument(
        "--single-window",
        action="store_true",
        help="Send the whole melody in one prompt. By default Add BV uses vocal windows.",
    )
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    request = json.loads(args.input_json.expanduser().read_text(encoding="utf-8"))
    result = generate(request, args)
    print(json.dumps(result, ensure_ascii=False))


def generate(request: dict[str, Any], args: argparse.Namespace) -> dict[str, Any]:
    adapter_path = args.adapter_path.expanduser()
    if adapter_path.is_file():
        adapter_path = adapter_path.parent
    if not adapter_path.exists():
        raise SystemExit(f"LoRA adapter not found: {args.adapter_path.expanduser()}")
    args.adapter_path = adapter_path

    model, tokenizer = mlx_load(args.model, adapter_path=str(args.adapter_path.expanduser()))
    return generate_with_model(request, args, model, tokenizer)


def generate_with_model(
    request: dict[str, Any],
    args: argparse.Namespace,
    model: Any,
    tokenizer: Any,
    stream_callback: Callable[[dict[str, Any]], None] | None = None,
) -> dict[str, Any]:
    style = style_by_id(str(request.get("style_id", "")))
    if style is None:
        raise SystemExit(f"Unknown style_id: {request.get('style_id')}")

    tempo_segments = normalize_tempos(request.get("tempo_segments"), float(request.get("bpm") or 120.0), float(request.get("duration") or 0.0))
    beat_map = build_beat_map(tempo_segments)
    lead = normalize_lead(request.get("lead_notes"), beat_map)
    chords = normalize_chords(request.get("chords"), beat_map)
    if not lead:
        raise SystemExit("No lead notes were provided")
    if not chords:
        raise SystemExit("No chords were provided")

    windows = [lead] if args.single_window else build_windows(lead, max(1, args.max_lead_notes), args.max_gap_beats)
    generated_sets = []
    warnings = []
    for index, window in enumerate(windows, start=1):
        prompt = render_prompt(request, style, chords, window, index, args.chord_padding_beats)
        raw = run_model(prompt, args, model, tokenizer)
        dump_debug(args, index, "prompt", prompt)
        dump_debug(args, index, "raw", raw)
        try:
            notes = validate_output(raw, style, window)
        except (Exception, SystemExit) as exc:
            warnings.append(f"window {index}: model YAML was not usable ({exc}); requesting repair")
            repair_prompt = render_repair_prompt(request, style, chords, window, [], index, args.chord_padding_beats, str(exc))
            repair_raw = run_model(repair_prompt, args, model, tokenizer)
            dump_debug(args, index, "repair_prompt", repair_prompt)
            dump_debug(args, index, "repair_raw", repair_raw)
            try:
                notes = validate_output(repair_raw, style, window)
            except (Exception, SystemExit) as repair_exc:
                warnings.append(f"window {index}: repair failed ({repair_exc}); used deterministic missing-note fallback")
                notes = []

        missing = missing_lead_refs(notes, window) if requires_one_to_one(style["id"]) else []
        if missing:
            repair_prompt = render_repair_prompt(request, style, chords, window, missing, index, args.chord_padding_beats, "missing source_lead_ref entries")
            repair_raw = run_model(repair_prompt, args, model, tokenizer)
            dump_debug(args, index, "missing_repair_prompt", repair_prompt)
            dump_debug(args, index, "missing_repair_raw", repair_raw)
            try:
                notes = merge_notes_by_source_ref(notes, validate_output(repair_raw, style, window))
            except (Exception, SystemExit) as repair_exc:
                warnings.append(f"window {index}: missing-note repair failed ({repair_exc}); filled missing notes deterministically")

        missing = missing_lead_refs(notes, window) if requires_one_to_one(style["id"]) else []
        if missing:
            notes = merge_notes_by_source_ref(notes, fallback_notes(style, missing))
            warnings.append(f"window {index}: filled {len(missing)} missing backing notes deterministically")

        generated_sets.append(notes)
        if stream_callback is not None:
            stream_callback(
                {
                    "type": "window",
                    "style_id": style["id"],
                    "style_name": style["name"],
                    "window_index": index,
                    "window_count": len(windows),
                    "note_count": sum(len(window_notes) for window_notes in generated_sets),
                    "warnings": warnings,
                    "notes": finalize_generated_notes(generated_sets, style, chords, str(request.get("key") or "C major"), beat_map),
                }
            )

    corrected = harmonic_postprocess(flatten_note_windows(generated_sets), chords, str(request.get("key") or "C major"), style["id"])
    debug_dsl_path = write_debug_dsl(request, style, chords, lead, [corrected], warnings)
    notes = stitch_notes([corrected], style, beat_map)
    return {
        "style_id": style["id"],
        "style_name": style["name"],
        "window_count": len(windows),
        "note_count": len(notes),
        "warnings": warnings,
        "debug_dsl_path": str(debug_dsl_path) if debug_dsl_path is not None else "",
        "notes": notes,
    }


def normalize_tempos(items: Any, fallback_bpm: float, duration: float) -> list[dict[str, float]]:
    tempos = []
    for item in items if isinstance(items, list) else []:
        if not isinstance(item, dict):
            continue
        start = float(item.get("start") or 0.0)
        end = float(item.get("end") or duration or start)
        bpm = max(20.0, min(300.0, float(item.get("bpm") or fallback_bpm)))
        if end > start:
            tempos.append({"start": start, "end": end, "bpm": bpm})
    if not tempos:
        tempos.append({"start": 0.0, "end": max(duration, 1.0), "bpm": max(20.0, min(300.0, fallback_bpm))})
    return sorted(tempos, key=lambda item: item["start"])


def build_beat_map(tempos: list[dict[str, float]]) -> list[BeatPoint]:
    points = []
    beat = 0.0
    previous_end = 0.0
    previous_bpm = tempos[0]["bpm"]
    for tempo in tempos:
        start = float(tempo["start"])
        if start > previous_end:
            beat += (start - previous_end) * previous_bpm / 60.0
        points.append(BeatPoint(time=start, beat=beat, bpm=float(tempo["bpm"])))
        end = float(tempo["end"])
        if end > start:
            beat += (end - start) * float(tempo["bpm"]) / 60.0
            previous_end = end
            previous_bpm = float(tempo["bpm"])
    return points


def time_to_beat(time: float, beat_map: list[BeatPoint]) -> float:
    point = beat_map[0]
    for candidate in beat_map:
        if candidate.time <= time:
            point = candidate
        else:
            break
    return point.beat + (time - point.time) * point.bpm / 60.0


def beat_to_time(beat: float, beat_map: list[BeatPoint]) -> float:
    point = beat_map[0]
    for candidate in beat_map:
        if candidate.beat <= beat:
            point = candidate
        else:
            break
    return point.time + (beat - point.beat) * 60.0 / point.bpm


def normalize_lead(items: Any, beat_map: list[BeatPoint]) -> list[dict[str, Any]]:
    output = []
    for index, item in enumerate(items if isinstance(items, list) else [], start=1):
        if not isinstance(item, dict):
            continue
        start_time = float(item.get("start") or 0.0)
        end_time = max(start_time + 0.03, float(item.get("end") or start_time + 0.25))
        start_beat = time_to_beat(start_time, beat_map)
        end_beat = time_to_beat(end_time, beat_map)
        # Backing composition uses the canonical piano-roll note. The detailed
        # pitch contour belongs to vocal rendering, not melodic generation.
        midi = int(round(float(item.get("pitch", item.get("pitch_exact", 60)))))
        output.append(
            {
                "id": f"lead_{index:04d}",
                "source_annotation_id": str(item.get("id") or f"lead_{index:04d}"),
                "start": round(start_beat, 5),
                "duration": round(max(0.05, end_beat - start_beat), 5),
                "bar": int(start_beat // 4) + 1,
                "beat": round(start_beat % 4 + 1, 5),
                "pitch": midi_to_pitch(midi),
                "midi_note": midi,
                "velocity": 95,
                "syllable": item.get("lyric") or None,
                "syllable_id": str(item.get("syllable_id") or ""),
                "legato_from_previous": bool(item.get("legato_from_previous")),
                "legato_to_next": bool(item.get("legato_to_next")),
                "melisma_continuation": bool(item.get("melisma_continuation")),
                "phrase_id": str(item.get("phrase_id") or "phrase_001"),
                "chord_ref": str(item.get("chord_ref") or chord_ref_at_beat(start_beat)),
            }
        )
    return sorted(output, key=lambda item: (item["start"], item["id"]))


def normalize_chords(items: Any, beat_map: list[BeatPoint]) -> list[dict[str, Any]]:
    output = []
    for index, item in enumerate(items if isinstance(items, list) else [], start=1):
        if not isinstance(item, dict):
            continue
        start_beat = time_to_beat(float(item.get("start") or 0.0), beat_map)
        end_beat = time_to_beat(float(item.get("end") or float(item.get("start") or 0.0) + 1.0), beat_map)
        name = str(item.get("name") or "C")
        root = chord_root(name)
        output.append(
            {
                "id": chord_id(index),
                "start": round(start_beat, 5),
                "duration": round(max(0.05, end_beat - start_beat), 5),
                "bar": int(start_beat // 4) + 1,
                "beat": round(start_beat % 4 + 1, 5),
                "chord": name,
                "root": root,
                "quality": chord_quality(name),
                "bass": root,
                "notes": chord_notes(name),
                "confidence": float(item.get("confidence") or 0.5),
            }
        )
    return sorted(output, key=lambda item: (item["start"], item["id"]))


def chord_id(index: int) -> str:
    return f"chord_{index:04d}"


def chord_ref_at_beat(beat: float) -> str:
    # Repaired later by render_prompt once selected chords are known.
    return "chord_0001"


def build_windows(lead: list[dict[str, Any]], max_lead_notes: int, max_gap: float) -> list[list[dict[str, Any]]]:
    windows = []
    current: list[dict[str, Any]] = []
    for note in lead:
        previous = current[-1] if current else None
        gap = note["start"] - (previous["start"] + previous["duration"]) if previous else 0.0
        phrase_changed = bool(previous and note["phrase_id"] != previous["phrase_id"])
        if current and (len(current) >= max_lead_notes or gap > max_gap or phrase_changed):
            windows.append(current)
            current = []
        current.append(note)
    if current:
        windows.append(current)
    return windows


def render_prompt(request: dict[str, Any], style: dict[str, str], chords: list[dict[str, Any]], window: list[dict[str, Any]], index: int, padding: float) -> str:
    start = window[0]["start"]
    end = max(note["start"] + note["duration"] for note in window)
    selected_chords = [chord for chord in chords if chord["start"] + chord["duration"] >= start - padding and chord["start"] <= end + padding]
    if not selected_chords:
        selected_chords = chords
    for note in window:
        note["chord_ref"] = chord_at(selected_chords, float(note["start"]))["id"]
    source = {
        "meta": {
            "title": request.get("title") or "Vocal Annotation Tool",
            "tempo": round(float(request.get("bpm") or 120.0)),
            "meter": request.get("meter") or "4/4",
            "key": request.get("key") or "C major",
            "ticks_per_beat": 480,
            "source_resolution": "seconds_converted_to_beats",
        },
        "tracks": {
            "chords": selected_chords,
            "lead_vocal": [{key: value for key, value in note.items() if key != "source_annotation_id"} for note in window],
        },
    }
    return (
        "TASK: GENERATE_BACKING_VOCAL\n"
        f"REQUESTED_STYLE_ID: {style['id']}\n"
        f"REQUESTED_STYLE_NAME: {style['name']}\n"
        "SOURCE_FILE: vocal_annotation_tool\n"
        f"WINDOW_INDEX: {index}\n"
        f"WINDOW_START_BEAT: {start:g}\n"
        f"WINDOW_END_BEAT: {end:g}\n\n"
        + yaml.safe_dump(source, allow_unicode=True, sort_keys=False)
    )


def render_repair_prompt(
    request: dict[str, Any],
    style: dict[str, str],
    chords: list[dict[str, Any]],
    window: list[dict[str, Any]],
    missing: list[dict[str, Any]],
    index: int,
    padding: float,
    reason: str,
) -> str:
    selected = missing if missing else window
    base = render_prompt(request, style, chords, selected, index, padding)
    refs = ", ".join(note["id"] for note in selected)
    return (
        base
        + "\nREPAIR_REQUEST:\n"
        + f"  reason: {reason}\n"
        + f"  Return notes only for these source_lead_ref values: [{refs}]\n"
        + "  Do not omit any requested source_lead_ref.\n"
    )


def run_model(prompt: str, args: argparse.Namespace, model: Any, tokenizer: Any) -> str:
    messages = [
        {"role": "system", "content": SYSTEM_PROMPT},
        {"role": "user", "content": prompt},
    ]
    try:
        formatted_prompt = tokenizer.apply_chat_template(
            messages,
            tokenize=False,
            add_generation_prompt=True,
            enable_thinking=False,
        )
    except TypeError:
        formatted_prompt = tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True)

    sampler = make_sampler(temp=max(0.0, float(args.temperature)), top_p=max(0.0, float(args.top_p)))
    return clean_generation(
        mlx_generate(
            model,
            tokenizer,
            formatted_prompt,
            max_tokens=max(64, int(args.max_tokens)),
            sampler=sampler,
            verbose=False,
        )
    )


def clean_generation(text: str) -> str:
    lines = []
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("Fetching ") or stripped in {"<think>", "</think>"} or stripped.startswith("```"):
            continue
        lines.append(line)
    cleaned = "\n".join(lines).strip()
    tracks_index = cleaned.find("tracks:")
    if tracks_index > 0:
        cleaned = cleaned[tracks_index:]
    return cleaned


def dump_debug(args: argparse.Namespace, window_index: int, name: str, text: str) -> None:
    if args.debug_dir is None:
        return
    path = args.debug_dir.expanduser()
    path.mkdir(parents=True, exist_ok=True)
    (path / f"window_{window_index:03d}_{name}.txt").write_text(text, encoding="utf-8")


def style_requirements(style_id: str) -> str:
    requirements = {
        "UNISON": "Double the lead melody at unison. Keep one note per lead note.",
        "OCT_UP": "Octave-double the lead one octave above when in vocal range; keep the lead contour.",
        "OCT_DOWN": "Octave-double the lead one octave below when in vocal range; keep the lead contour.",
        "THIRD_UP": "Write chord-aware upper thirds. Prefer diatonic/chord tones a third above each lead note. Avoid unison/common-tone laziness unless no singable upper third exists. Follow lead contour instead of sitting on one pitch.",
        "THIRD_DOWN": "Write chord-aware lower thirds. Prefer diatonic/chord tones a third below each lead note. Avoid unison/common-tone laziness unless no singable lower third exists.",
        "SIXTH_UP": "Write upper sixth harmony with smooth voice leading and range correction; avoid static drones.",
        "SIXTH_DOWN": "Write lower sixth harmony with smooth voice leading and range correction; avoid static drones.",
        "FIFTH_UP": "Write fifth-above harmony but soften to nearby chord tones when parallel fifths sound exposed.",
        "FIFTH_DOWN": "Write fifth-below harmony but soften to nearby chord tones when parallel fifths sound exposed.",
        "FOURTH_UP": "Use upper fourths as a color interval, resolving obvious suspensions to chord tones.",
        "FOURTH_DOWN": "Use lower fourths as a color interval, resolving obvious suspensions to chord tones.",
        "DRONE_ROOT": "Use sustained phrase-level chord roots. It is allowed to use fewer/longer notes than lead.",
        "DRONE_FIFTH": "Use sustained phrase-level chord fifths. It is allowed to use fewer/longer notes than lead.",
        "DRONE_THIRD": "Use sustained phrase-level chord thirds. It is allowed to use fewer/longer notes than lead.",
        "PEDAL_ROOT": "Use a long tonic/chord-root pedal. It is allowed to use fewer/longer notes than lead.",
        "PEDAL_FIFTH": "Use a long fifth pedal. It is allowed to use fewer/longer notes than lead.",
        "CONTRARY": "Prefer contrary motion against the lead contour while staying chord-aware and singable.",
        "OBLIQUE": "Use oblique motion intentionally: hold common tones only when they create a real harmony, not as a lazy default.",
        "PAR3": "Use mostly parallel chord-aware thirds; avoid unison and avoid long static common tones.",
        "PAR6": "Use mostly parallel chord-aware sixths; avoid unison and avoid long static common tones.",
        "CHOIR_SOP": "Write a singable SATB soprano line above the lead context, favor chord tones and smooth steps.",
        "CHOIR_ALT": "Write a singable SATB alto inner line, favor chord tones and smooth voice leading.",
        "CHOIR_TEN": "Write a singable SATB tenor line below/around the lead, favor chord tones and smooth voice leading.",
        "CHOIR_BASS": "Write a bass-support line using roots/fifths and conservative voice leading.",
        "HARMONY_2P": "Write natural two-part backing around the lead with chord-aware thirds/sixths and smooth contour.",
        "HARMONY_3P": "Write one useful line for a three-part harmony texture, choosing chord tones with smooth voice leading.",
        "HARMONY_4P": "Write one useful SATB-style line for four-part harmony, conservative and chord-aware.",
        "STYLE_POP": "Modern pop backing: thirds/sixths, smooth motion, restrained tensions, no lazy unison unless intended.",
        "STYLE_FOLK": "Folk harmony: open consonant intervals, fifths/thirds/drones where musically justified.",
        "STYLE_GOSPEL": "Gospel harmony: strong chord tones, tasteful color tones, smooth resolution of tensions.",
        "STYLE_CLASSICAL": "Classical choral counterpoint: avoid parallel exposed fifths/octaves, resolve tendency tones.",
        "STYLE_BSHOP": "Barbershop-like close harmony: chord tones/sevenths with smooth contrary/oblique motion.",
        "SUSP": "Use prepared suspensions that resolve; do not leave dissonances hanging.",
        "PASSING": "Use passing tones between stable chord tones; long notes should be stable chord/scale tones.",
        "TENSION_RES": "Create intentional tension that resolves to stable chord tones within the phrase.",
        "DYNAMIC_CP": "Write varied phrase-level counterpoint. Alternate parallel, contrary, and oblique motion; vary the lead interval and avoid repeating either one pitch or one interval for the whole phrase.",
    }
    return requirements.get(style_id, "Write a musical chord-aware backing vocal line with smooth voice leading.")


def validate_output(text: str, style: dict[str, str], source_lead: list[dict[str, Any]]) -> list[dict[str, Any]]:
    parsed = yaml.safe_load(text)
    backing = parsed.get("tracks", {}).get("backing_vocals", []) if isinstance(parsed, dict) else []
    if not isinstance(backing, list) or len(backing) != 1:
        raise SystemExit("model returned invalid tracks.backing_vocals")
    lead_by_id = {note["id"]: note for note in source_lead}
    lead_by_start = sorted(source_lead, key=lambda item: item["start"])
    notes = []
    seen_refs: set[str] = set()
    for part_index, part in enumerate(backing[0].get("parts", []) or [], start=1):
        for note_index, note in enumerate(part.get("notes", []) or [], start=1):
            if not isinstance(note, dict):
                continue
            source = lead_by_id.get(str(note.get("source_lead_ref") or ""))
            if source is None:
                if "lead_index" in note:
                    try:
                        lead_index = int(note["lead_index"])
                        if 1 <= lead_index <= len(lead_by_start):
                            source = lead_by_start[lead_index - 1]
                        elif 0 <= lead_index < len(lead_by_start):
                            source = lead_by_start[lead_index]
                    except (TypeError, ValueError):
                        source = None
                if source is None and "start" in note:
                    note_start = float(note.get("start", source_lead[0]["start"]))
                    source = min(source_lead, key=lambda lead_note: abs(float(lead_note["start"]) - note_start))
            if source is None:
                continue
            if source["id"] in seen_refs:
                continue
            seen_refs.add(source["id"])
            midi = int(note.get("midi_note") or pitch_to_midi(str(note.get("pitch") or "")) or source["midi_note"])
            midi = max(24, min(96, midi))
            notes.append(
                {
                    "id": f"{style['id'].lower()}_{part_index:02d}_{note_index:04d}",
                    "part": str(part.get("role") or part.get("id") or f"part_{part_index:02d}"),
                    "strategy": str(part.get("strategy") or style["id"].lower()),
                    "start": float(source["start"]),
                    "duration": max(0.05, float(source["duration"])),
                    "midi_note": midi,
                    "pitch": midi_to_pitch(midi),
                    "velocity": int(note.get("velocity") or 74),
                    "lyric": source.get("syllable") or "",
                    "syllable_id": source.get("syllable_id") or "",
                    "legato_from_previous": bool(source.get("legato_from_previous")),
                    "legato_to_next": bool(source.get("legato_to_next")),
                    "melisma_continuation": bool(source.get("melisma_continuation")),
                    "source_lead_ref": source["id"],
                }
            )
    if not notes:
        raise SystemExit("model returned no valid backing notes")
    return notes


def missing_lead_refs(notes: list[dict[str, Any]], source_lead: list[dict[str, Any]]) -> list[dict[str, Any]]:
    generated_refs = {str(note.get("source_lead_ref") or "") for note in notes}
    return [source for source in source_lead if source["id"] not in generated_refs]


def merge_notes_by_source_ref(existing: list[dict[str, Any]], additions: list[dict[str, Any]]) -> list[dict[str, Any]]:
    by_ref = {str(note.get("source_lead_ref") or ""): note for note in existing}
    for note in additions:
        ref = str(note.get("source_lead_ref") or "")
        if ref and ref not in by_ref:
            by_ref[ref] = note
    return list(by_ref.values())


def requires_one_to_one(style_id: str) -> bool:
    return not (style_id.startswith("DRONE_") or style_id.startswith("PEDAL_"))


def fallback_notes(style: dict[str, str], source_lead: list[dict[str, Any]]) -> list[dict[str, Any]]:
    interval = fallback_interval(style["id"])
    output = []
    for index, source in enumerate(source_lead, start=1):
        midi = int(source["midi_note"]) + interval
        midi = max(36, min(84, midi))
        output.append(
            {
                "id": f"{style['id'].lower()}_fallback_{index:04d}",
                "part": "fallback",
                "strategy": style["id"].lower(),
                "start": float(source["start"]),
                "duration": max(0.05, float(source["duration"])),
                "midi_note": midi,
                "pitch": midi_to_pitch(midi),
                "velocity": 70,
                "lyric": source.get("syllable") or "",
                "syllable_id": source.get("syllable_id") or "",
                "legato_from_previous": bool(source.get("legato_from_previous")),
                "legato_to_next": bool(source.get("legato_to_next")),
                "melisma_continuation": bool(source.get("melisma_continuation")),
                "source_lead_ref": source["id"],
            }
        )
    return output


def fallback_interval(style_id: str) -> int:
    intervals = {
        "UNISON": 0,
        "OCT_UP": 12,
        "OCT_DOWN": -12,
        "THIRD_UP": 4,
        "THIRD_DOWN": -3,
        "SIXTH_UP": 9,
        "SIXTH_DOWN": -8,
        "FIFTH_UP": 7,
        "FIFTH_DOWN": -5,
        "FOURTH_UP": 5,
        "FOURTH_DOWN": -7,
        "PAR3": 4,
        "PAR6": 9,
        "CHOIR_SOP": 7,
        "CHOIR_ALT": 4,
        "CHOIR_TEN": -5,
        "CHOIR_BASS": -12,
        "HARMONY_2P": 4,
        "HARMONY_3P": 7,
        "HARMONY_4P": -5,
        "STYLE_POP": 4,
        "STYLE_FOLK": 7,
        "STYLE_GOSPEL": 4,
        "STYLE_CLASSICAL": -3,
        "STYLE_BSHOP": 4,
        "SUSP": 5,
        "PASSING": 2,
        "TENSION_RES": 4,
    }
    if style_id.startswith("DRONE_") or style_id.startswith("PEDAL_"):
        return -12
    return intervals.get(style_id, 4)


def stitch_notes(windows: list[list[dict[str, Any]]], style: dict[str, str], beat_map: list[BeatPoint]) -> list[dict[str, Any]]:
    output = []
    for window in windows:
        output.extend(dict(note) for note in window)
    output.sort(key=lambda note: (note["start"], note["midi_note"], note["source_lead_ref"]))
    for index, note in enumerate(output, start=1):
        start_time = beat_to_time(float(note["start"]), beat_map)
        end_time = beat_to_time(float(note["start"]) + float(note["duration"]), beat_map)
        note["id"] = f"bv_{style['id'].lower()}_{index:04d}"
        note["start"] = round(start_time, 6)
        note["end"] = round(max(start_time + 0.03, end_time), 6)
    return output


def flatten_note_windows(windows: list[list[dict[str, Any]]]) -> list[dict[str, Any]]:
    notes = [dict(note) for window in windows for note in window]
    return sorted(notes, key=lambda note: (float(note["start"]), int(note["midi_note"]), str(note.get("source_lead_ref") or "")))


def finalize_generated_notes(
    windows: list[list[dict[str, Any]]],
    style: dict[str, str],
    chords: list[dict[str, Any]],
    key: str,
    beat_map: list[BeatPoint],
) -> list[dict[str, Any]]:
    corrected = harmonic_postprocess(flatten_note_windows(windows), chords, key, style["id"])
    return stitch_notes([corrected], style, beat_map)


def harmonic_postprocess(
    notes: list[dict[str, Any]],
    chords: list[dict[str, Any]],
    key: str,
    style_id: str,
) -> list[dict[str, Any]]:
    """Snap generated pitches to the nearest harmonically valid pitch class."""
    # Exact doublings must preserve the lead, including intentional chromatic notes.
    if style_id in {"UNISON", "OCT_UP", "OCT_DOWN"}:
        return [dict(note) for note in notes]

    scale_pitch_classes = key_pitch_classes(key)
    allow_scale_tensions = style_id in {"SUSP", "PASSING", "TENSION_RES"}
    corrected: list[dict[str, Any]] = []
    previous_pitch: int | None = None

    for source in notes:
        note = dict(source)
        midi = int(note["midi_note"])
        chord = chord_at(chords, float(note["start"])) if chords else {}
        chord_pitch_classes = {
            parsed % 12
            for pitch in chord.get("notes", []) or []
            if (parsed := pitch_to_midi(str(pitch))) is not None
        }

        if allow_scale_tensions:
            allowed = chord_pitch_classes | scale_pitch_classes
        else:
            # The current chord is authoritative, including borrowed chords that
            # intentionally contain tones outside the global key.
            allowed = chord_pitch_classes or scale_pitch_classes

        corrected_midi = nearest_allowed_midi(midi, allowed, previous_pitch)
        if corrected_midi != midi:
            note["original_midi_note"] = midi
            note["harmonic_correction"] = corrected_midi - midi
            note["midi_note"] = corrected_midi
            note["pitch"] = midi_to_pitch(corrected_midi)
        corrected.append(note)
        previous_pitch = corrected_midi

    return corrected


def key_pitch_classes(key: str) -> set[int]:
    match = re.match(r"\s*([A-Ga-g](?:#|b)?)", key)
    if match is None:
        return set()
    root_name = match.group(1)[0].upper() + match.group(1)[1:]
    root = NOTE_TO_PC.get(root_name)
    if root is None:
        return set()
    intervals = (0, 2, 3, 5, 7, 8, 10) if "minor" in key.lower() else (0, 2, 4, 5, 7, 9, 11)
    return {(root + interval) % 12 for interval in intervals}


def nearest_allowed_midi(midi: int, allowed_pitch_classes: set[int], previous_pitch: int | None) -> int:
    if not allowed_pitch_classes or midi % 12 in allowed_pitch_classes:
        return midi
    candidates = [
        candidate
        for candidate in range(max(24, midi - 6), min(96, midi + 6) + 1)
        if candidate % 12 in allowed_pitch_classes
    ]
    if not candidates:
        return midi
    return min(
        candidates,
        key=lambda candidate: (
            abs(candidate - midi),
            abs(candidate - previous_pitch) if previous_pitch is not None else 0,
            0 if candidate >= midi else 1,
        ),
    )


def write_debug_dsl(
    request: dict[str, Any],
    style: dict[str, str],
    chords: list[dict[str, Any]],
    lead: list[dict[str, Any]],
    generated_sets: list[list[dict[str, Any]]],
    warnings: list[str],
) -> Path | None:
    generated = []
    for window in generated_sets:
        generated.extend(window)
    generated.sort(key=lambda note: (float(note["start"]), str(note.get("source_lead_ref", "")), int(note["midi_note"])))

    backing_notes = []
    lead_by_id = {note["id"]: note for note in lead}
    for index, note in enumerate(generated, start=1):
        source = lead_by_id.get(str(note.get("source_lead_ref") or ""))
        start = float(note["start"])
        duration = max(0.05, float(note["duration"]))
        backing_notes.append(
            {
                "id": f"{style['id'].lower()}_{index:04d}",
                "start": round(start, 5),
                "duration": round(duration, 5),
                "bar": int(start // 4) + 1,
                "beat": round(start % 4 + 1, 5),
                "pitch": midi_to_pitch(int(note["midi_note"])),
                "midi_note": int(note["midi_note"]),
                "velocity": int(note.get("velocity") or 74),
                "syllable": source.get("syllable") if source else None,
                "phrase_id": source.get("phrase_id") if source else "phrase_001",
                "chord_ref": source.get("chord_ref") if source else chord_ref_at_beat(start),
                "source_lead_ref": str(note.get("source_lead_ref") or ""),
            }
        )

    lead_notes = []
    for note in lead:
        lead_notes.append(
            {
                "id": note["id"],
                "start": note["start"],
                "duration": note["duration"],
                "bar": note["bar"],
                "beat": note["beat"],
                "pitch": note["pitch"],
                "midi_note": note["midi_note"],
                "velocity": note["velocity"],
                "syllable": note["syllable"],
                "phrase_id": note["phrase_id"],
                "chord_ref": note["chord_ref"],
                "source_annotation_id": note["source_annotation_id"],
            }
        )

    dsl = {
        "meta": {
            "title": f"Vocal Annotation Tool BV Debug - {style['id']}",
            "tempo": round(float(request.get("bpm") or 120.0)),
            "meter": request.get("meter") or "4/4",
            "key": request.get("key") or "C major",
            "ticks_per_beat": 480,
            "source_resolution": "seconds_converted_to_beats",
            "requested_style_id": style["id"],
            "requested_style_name": style["name"],
            "source_file": request.get("source_file") or "vocal_annotation_tool",
        },
        "tracks": {
            "chords": chords,
            "lead_vocal": lead_notes,
            "backing_vocals": [
                {
                    "id": style["id"],
                    "name": style["name"],
                    "description": style.get("description", ""),
                    "confidence": 0.5,
                    "parts": [
                        {
                            "id": f"{style['id'].lower()}_part_01",
                            "role": "backing",
                            "strategy": style["id"].lower(),
                            "range": "C3-C6",
                            "notes": backing_notes,
                        }
                    ],
                }
            ],
        },
        "analysis": {
            "warnings": warnings,
            "lead_detection": {
                "notes": len(lead_notes),
            },
            "chord_detection": {
                "chords": len(chords),
            },
        },
    }

    output_dir = REPO_ROOT / "tools" / "dsl_player" / "generated"
    output_dir.mkdir(parents=True, exist_ok=True)
    output_path = output_dir / "latest_backing_debug.yaml"
    output_path.write_text(
        yaml.dump(dsl, Dumper=IndentedSafeDumper, allow_unicode=True, sort_keys=False),
        encoding="utf-8",
    )
    return output_path


def style_by_id(style_id: str) -> dict[str, str] | None:
    return next((style for style in STYLE_DEFS if style["id"] == style_id), None)


def chord_at(chords: list[dict[str, Any]], beat: float) -> dict[str, Any]:
    for chord in chords:
        if chord["start"] <= beat < chord["start"] + chord["duration"]:
            return chord
    return chords[-1]


def pitch_to_midi(pitch: str) -> int | None:
    match = re.fullmatch(r"([A-G](?:#|b)?)(-?\d+)", pitch.strip())
    if not match:
        return None
    note, octave = match.groups()
    return (int(octave) + 1) * 12 + NOTE_TO_PC[note]


def midi_to_pitch(midi: int) -> str:
    return f"{PC_TO_NOTE[midi % 12]}{midi // 12 - 1}"


def chord_root(name: str) -> str:
    match = re.match(r"([A-G](?:#|b)?)", name.strip())
    return match.group(1) if match else "C"


def chord_quality(name: str) -> str:
    lowered = name.lower()
    if "dim" in lowered:
        return "diminished"
    if "aug" in lowered:
        return "augmented"
    if "m" in lowered and "maj" not in lowered:
        return "minor"
    return "major"


def chord_notes(name: str) -> list[str]:
    root = chord_root(name)
    root_pc = NOTE_TO_PC.get(root, 0)
    quality = chord_quality(name)
    intervals = (0, 3, 7) if quality == "minor" else (0, 4, 7)
    return [midi_to_pitch(48 + ((root_pc + interval - 0) % 12)) for interval in intervals]


if __name__ == "__main__":
    main()
