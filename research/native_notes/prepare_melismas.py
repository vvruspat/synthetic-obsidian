"""Controlled MIDI / OpenUtau vowel melismas; synthetic labels, never human labels."""
from __future__ import annotations

import argparse
import hashlib
import json
import math
from pathlib import Path

import mido
import numpy as np
import soundfile as sf

from .synthesis_manifest import SCHEMA

POLICY = "openutau-diffsinger-vowel-phrases-v1"
VOWELS = {"a": "а", "o": "о", "u": "у", "e": "э"}
BANKS = {
    "pjs": {"singer": "PJS DiffSinger", "phonemes": dict(a="a", o="o", u="u", e="e"),
            "source": "https://github.com/UtaUtaUtau/pjs-manual-labels/releases/tag/diffsinger",
            "license": "CC-BY-SA-4.0", "register": "male", "root_offset": 0, "range": [43, 70]},
    "qixuan": {"singer": "Qixuan_v2.7.0_DiffSinger_OpenUtau",
               "phonemes": dict(a="zh/a", o="zh/o", u="zh/u", e="ja/e"),
               "source": "https://github.com/yqzhishen/qixuan-diffsinger/releases/tag/v2.7.0",
               "license": "Qixuan-2.7.0-custom-terms-boundary-analysis-only",
               "register": "female", "root_offset": 12, "range": [55, 82]},
}


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, document):
    path = Path(path)
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(document, indent=2, ensure_ascii=False, allow_nan=False) + "\n")
    temporary.replace(path)


def composition(index, seed):
    rng = np.random.default_rng(np.random.SeedSequence([seed, index]))
    category = ("legato",) * 5 + ("mixed_phrases",) * 2 + ("reattacks", "held_vibrato", "edge_drift")
    category = category[index % len(category)]
    bpm = int(rng.choice([65, 80, 95, 110, 130, 160]))
    ticks_per_second = bpm * 8
    major = int(rng.choice([0, 1])) == 0
    scale = np.array([0, 2, 4, 5, 7, 9, 11]) if major else np.array([0, 2, 3, 5, 7, 8, 10])
    root = int(rng.choice([44, 46, 48, 50, 52, 55]))
    groups = int(rng.integers(2, 4)) if category == "mixed_phrases" else (int(rng.integers(4, 7)) if category == "reattacks" else 1)
    cursor = int(round(rng.uniform(.28, .55) * ticks_per_second))
    notes = []
    repeated_tone = root + int(scale[2])
    for phrase in range(groups):
        count = int(rng.integers(4, 10)) if category in ("legato", "mixed_phrases") else 1
        degree = int(rng.integers(1, 5))
        for number in range(count):
            if number:
                degree = int(np.clip(degree + rng.choice([-2, -1, 1, 2], p=[.1, .4, .4, .1]), 0, 9))
            tone = root + int(scale[degree % 7]) + 12 * (degree // 7)
            if category == "reattacks":
                tone = repeated_tone if phrase < 2 or rng.random() < .6 else repeated_tone + int(rng.choice([-2, 2]))
            duration = int(rng.choice([120, 240, 360, 480, 720, 960], p=[.12, .28, .15, .25, .13, .07]))
            duration = max(duration, int(math.ceil(.21 * ticks_per_second)))
            if category in ("held_vibrato", "edge_drift"):
                duration = int(round(rng.uniform(2.5, 5.5) * ticks_per_second))
            notes.append({"position": cursor, "duration": duration, "tone": tone, "phrase": phrase})
            cursor += duration
        if phrase + 1 < groups:
            gap = rng.uniform(.09, .28) if category == "reattacks" else rng.uniform(.25, 1.1)
            cursor += int(round(gap * ticks_per_second))
    seconds = (cursor + int(round(rng.uniform(.35, .65) * ticks_per_second))) / ticks_per_second
    return {"score_index": index, "category": category, "bpm": bpm, "notes": notes,
            "seconds": seconds, "vibrato_hz": float(rng.uniform(4.0, 8.5)),
            "vibrato_semitones": float(rng.uniform(.15, .5) if category == "held_vibrato" else rng.uniform(.04, .32)),
            "phase": float(rng.uniform(0, 2 * np.pi)), "glide_seconds": float(rng.uniform(.035, .13)),
            "edge_drift_semitones": float(rng.choice([-1, 1]) * rng.uniform(.3, .9)) if category == "edge_drift" else 0.0}


def pitch_control(score, notes):
    times = np.arange(int(math.ceil(score["seconds"] * 100)) + 1) / 100
    pitch = np.full(len(times), float(notes[0]["tone"]))
    unit = score["bpm"] * 8
    for n in notes:
        pitch[times >= n["position"] / unit] = n["tone"]
    for previous, note in zip(notes, notes[1:]):
        if previous["phrase"] != note["phrase"]:
            continue
        boundary = note["position"] / unit
        width = min(score["glide_seconds"], .35 * min(previous["duration"], note["duration"]) / unit)
        ramp = (times >= boundary - width/2) & (times <= boundary + width/2)
        x = np.clip((times[ramp] - boundary + width/2) / width, 0, 1)
        pitch[ramp] = previous["tone"] + (note["tone"] - previous["tone"]) * (.5 - .5 * np.cos(np.pi * x))
    for note in notes:
        start, end = note["position"] / unit, (note["position"] + note["duration"]) / unit
        inside = (times >= start) & (times < end)
        envelope = np.clip(np.minimum(times[inside] - start, end - times[inside]) / .07, 0, 1)
        pitch[inside] += envelope * score["vibrato_semitones"] * np.sin(2*np.pi*score["vibrato_hz"]*times[inside] + score["phase"])
        if score["edge_drift_semitones"]:
            pitch[inside] += score["edge_drift_semitones"] * (
                np.clip(1 - (times[inside] - start) / .18, 0, 1)
                - np.clip(1 - (end - times[inside]) / .18, 0, 1))
    return np.round(pitch, 5).tolist()


def write_midi(path, job, vowel):
    midi = mido.MidiFile(ticks_per_beat=480)
    track = mido.MidiTrack()
    midi.tracks.append(track)
    track.append(mido.MetaMessage("set_tempo", tempo=mido.bpm2tempo(job["bpm"])))
    track.append(mido.MetaMessage("track_name", name=f"{job['singer']} vowel={vowel}"))
    events = []
    for note in job["notes"]:
        events.extend([(note["position"], 1, mido.Message("note_on", note=note["tone"], velocity=90)),
                       (note["position"] + note["duration"], 0, mido.Message("note_off", note=note["tone"], velocity=0))])
    previous = 0
    for time, _, message in sorted(events, key=lambda e: (e[0], e[1])):
        track.append(message.copy(time=time - previous))
        previous = time
    midi.save(path)


def plan(args):
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    if (root / "plan.json").exists():
        raise ValueError("A frozen plan already exists; render its jobs.json to resume")
    banks = {}
    for name, bank in BANKS.items():
        path = args.singers / bank["singer"]
        # Pin both render models and their interpretation, including dictionaries.
        files = {str(p.relative_to(path)): digest(p) for p in sorted(path.rglob("*"))
                 if p.is_file() and p.suffix.lower() in (".onnx", ".yaml", ".json", ".txt")}
        banks[name] = {**bank, "path": str(path.resolve()), "files_sha256": files}
    jobs, records = [], []
    for index in range(args.scores):
        score = composition(index, args.seed)
        for bank_name, bank in banks.items():
            for vowel in VOWELS:
                notes = [{**n, "tone": int(np.clip(n["tone"] + bank["root_offset"], *bank["range"]))} for n in score["notes"]]
                # A scale walk clipped at a range edge must not create an unarticulated
                # same-pitch "new note". Reflect that step into the available range.
                for a, b in zip(notes, notes[1:]):
                    if a["phrase"] == b["phrase"] and a["tone"] == b["tone"]:
                        b["tone"] += -1 if b["tone"] == bank["range"][1] else 1
                gender = [0, -20, 20][index % 3] if bank_name == "qixuan" else 0
                recipe = {"policy": POLICY, "seed": args.seed, "score": score, "bank": bank_name,
                          "bank_files_sha256": bank["files_sha256"], "vowel": vowel,
                          "notes": notes, "gender": gender, "steps": args.steps,
                          "pitch_midi": pitch_control(score, notes)}
                identity = hashlib.sha256(json.dumps(recipe, sort_keys=True, separators=(",", ":")).encode()).hexdigest()
                stem = f"melisma_{identity[:24]}"
                folder = root / "clips" / stem
                folder.mkdir(parents=True, exist_ok=False)
                prefix = folder / stem
                write_json(prefix.with_suffix(".recipe.json"), recipe)
                job = {"id": identity, "singer": bank["singer"], "phoneme": bank["phonemes"][vowel],
                       "bpm": score["bpm"], "seconds": score["seconds"], "notes": notes,
                       "pitch_midi": recipe["pitch_midi"], "output_prefix": str(prefix),
                       "steps": args.steps, "gender": gender}
                jobs.append(job)
                write_midi(prefix.with_suffix(".mid"), job, vowel)
                records.append({"id": identity, "stem": stem, "score_index": index,
                                "bank": bank_name, "vowel": vowel, "category": score["category"],
                                "seconds": score["seconds"], "prefix": str(prefix.relative_to(root)),
                                "recipe_sha256": digest(prefix.with_suffix(".recipe.json")),
                                "midi_sha256": digest(prefix.with_suffix(".mid"))})
    write_json(root / "jobs.json", jobs)
    write_json(root / "plan.json", {"schema": SCHEMA, "policy": POLICY, "seed": args.seed,
               "scores": args.scores, "banks": banks, "vowels": VOWELS, "records": records,
               "jobs_sha256": digest(root / "jobs.json"), "review_status": "synthetic_not_human_reviewed"})
    print(json.dumps({"clips": len(jobs), "hours": sum(j["seconds"] for j in jobs)/3600,
                      "notes": sum(len(j["notes"]) for j in jobs), "output": str(root)}))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--singers", type=Path, default=Path.home()/".synthetic_obsidian/models/diffsinger")
    parser.add_argument("--scores", type=int, default=64)
    parser.add_argument("--seed", type=int, default=2431802)
    parser.add_argument("--steps", type=int, default=20)
    args = parser.parse_args()
    if not 1 <= args.scores <= 10000 or not 1 <= args.steps <= 100:
        parser.error("Invalid synthesis size/steps")
    plan(args)


if __name__ == "__main__":
    main()
