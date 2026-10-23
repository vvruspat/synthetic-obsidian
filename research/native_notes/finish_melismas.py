"""Check controlled synthesis against independent native YIN; export portable labels."""
from __future__ import annotations

import argparse
from collections import Counter
from concurrent.futures import ThreadPoolExecutor
import csv
import json
from pathlib import Path
import subprocess

import numpy as np
import soundfile as sf

from .prepare_melismas import SCHEMA, digest, write_json
from .synthesis_manifest import QA_POLICY


def envelope(audio, rate):
    width, hop = round(rate * .02), round(rate * .005)
    padded = np.pad(np.square(audio, dtype=np.float64), (width//2, width//2))
    total = np.r_[0.0, np.cumsum(padded)]
    positions = np.arange(0, len(audio), hop)
    rms = np.sqrt(np.maximum(0, (total[positions+width] - total[positions]) / width))
    return positions / rate, rms


def pitch_crossing(times, pitch, voiced, nominal, before, after):
    """Locate a prescribed step's midpoint, not arbitrary changes in an unlabelled recording."""
    if before == after:
        return None
    smooth = np.median(np.pad(pitch, 1, mode="edge")[np.arange(len(pitch))[:, None] + np.arange(3)], axis=1)
    position = (smooth - before) / (after - before)
    crossings = np.flatnonzero((position[:-1] <= .5) & (position[1:] > .5)
                              & voiced[:-1] & voiced[1:]
                              & (times[:-1] >= nominal-.09) & (times[1:] <= nominal+.09))
    if not len(crossings):
        return None
    candidates = times[crossings] + np.diff(times)[crossings] * (
        (.5-position[crossings]) / (position[crossings+1]-position[crossings]))
    return float(min(candidates, key=lambda x: abs(x-nominal)))


def align_boundaries(job, audio, rate, frames):
    unit = job["bpm"] * 8
    notes = [{"id": f"n{i+1:04}", "start": n["position"]/unit,
              "end": (n["position"]+n["duration"])/unit, "pitch": n["tone"],
              "phrase": n["phrase"]} for i, n in enumerate(job["notes"])]
    shifts, ignore, reasons = [], [], []
    t, energy = envelope(audio, rate)
    groups = [[i for i, n in enumerate(notes) if n["phrase"] == phrase]
              for phrase in sorted({n["phrase"] for n in notes})]
    for group_index, indexes in enumerate(groups):
        first, last = notes[indexes[0]], notes[indexes[-1]]
        start, end = first["start"], last["end"]
        body = energy[(t >= start+.07) & (t < end-.07)]
        level = float(np.median(body)) if len(body) else 0.0
        threshold = max(1e-4, level*.1)
        left = max(start-.14, 0 if group_index == 0 else (notes[groups[group_index-1][-1]]["end"]+start)/2)
        right = min(end+.18, len(audio)/rate if group_index+1 == len(groups) else (end+notes[groups[group_index+1][0]]["start"])/2)
        on = np.flatnonzero((t >= left) & (t <= start+.14) & (energy >= threshold))
        off = np.flatnonzero((t >= end-.14) & (t <= right) & (energy >= threshold))
        # Two neighbouring 5-ms hops reject isolated noise excursions.
        on = on[(on+1 < len(energy)) & (np.r_[energy[1:], 0][on] >= threshold)]
        off = off[(off > 0) & (np.r_[0, energy[:-1]][off] >= threshold)]
        if not len(on) or not len(off) or level < .001:
            reasons.append("missing_phrase_energy")
            continue
        first["start"], last["end"] = float(t[on[0]]), float(t[off[-1]])
        shifts.extend([{"kind": "phrase_onset", "nominal": start, "aligned": first["start"]},
                       {"kind": "phrase_offset", "nominal": end, "aligned": last["end"]}])
    times = np.arange(len(frames)) / 100
    valid = (frames[:, 2] > .5) & (frames[:, 1] >= .65)
    for previous, note in zip(notes, notes[1:]):
        if previous["phrase"] != note["phrase"]:
            continue
        nominal = note["start"]
        aligned = pitch_crossing(times, frames[:, 0], valid, nominal, previous["pitch"], note["pitch"])
        if aligned is None:
            ignore.append([max(0, nominal-.12), min(job["seconds"], nominal+.12)])
            shifts.append({"kind": "unresolved_legato", "nominal": nominal, "aligned": None})
        else:
            previous["end"] = note["start"] = aligned
            shifts.append({"kind": "legato", "nominal": nominal, "aligned": aligned})
    for a, b in zip(notes, notes[1:]):
        if a["end"] > b["start"]:
            reasons.append("overlapping_aligned_notes")
        if a["phrase"] != b["phrase"]:
            window = energy[(t >= a["end"]) & (t < b["start"])]
            nearby = energy[(t >= a["end"]-.2) & (t < a["end"])]
            if b["start"]-a["end"] < .025 or not len(window) or not len(nearby) or window.min() > max(.001, np.median(nearby)*.25):
                reasons.append("no_audible_pause")
    if any(n["end"]-n["start"] < .07 for n in notes):
        reasons.append("aligned_note_too_short")
    legato = sum(s["kind"] in ("legato", "unresolved_legato") for s in shifts)
    if legato and len(ignore)/legato > .2:
        reasons.append("too_many_unresolved_transitions")
    return notes, shifts, ignore, reasons


def process(root, record, job, runner):
    prefix = root / record["prefix"]
    for suffix, key in ((".recipe.json", "recipe_sha256"), (".mid", "midi_sha256")):
        if digest(prefix.with_suffix(suffix)) != record[key]:
            raise ValueError("Frozen synthesis input changed")
    render = json.loads(prefix.with_suffix(".render.json").read_text())
    if render["id"] != record["id"]:
        raise ValueError("Render identity changed")
    raw = prefix.with_suffix(".raw.wav")
    audio, rate = sf.read(raw, dtype="float32")
    if rate != 44100 or audio.ndim != 1 or not np.isfinite(audio).all() or abs(len(audio)/rate-job["seconds"]) > 1/rate:
        raise ValueError("Invalid rendered audio/time grid")
    rms = float(np.sqrt(np.mean(audio.astype(np.float64)**2)))
    raw_peak = float(np.max(np.abs(audio)))
    raw_clipped = float(np.mean(np.abs(audio) >= .9998))
    if rms <= 1e-6:
        raise ValueError("Silent render")
    rng = np.random.default_rng(int(record["id"][:16], 16))
    gain = min(10**(rng.uniform(-23, -17)/20) / rms, .94/max(raw_peak, 1e-8))
    final = prefix.with_suffix(".wav")
    temporary = prefix.with_suffix(".wav.tmp")
    sf.write(temporary, audio*gain, rate, format="WAV", subtype="PCM_24")
    temporary.replace(final)
    diagnostic = prefix.with_suffix(".native-yin.json")
    subprocess.run([str(runner), "--predict", "math", str(final), str(diagnostic), "diagnostics"],
                   check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    analysis = json.loads(diagnostic.read_text())
    frames = np.array(analysis["yin_frames"], dtype=np.float64)
    times = np.arange(len(frames))/100
    expected = np.interp(times, np.arange(len(job["pitch_midi"]))/100, job["pitch_midi"])
    interior = np.zeros(len(times), bool)
    for n in job["notes"]:
        start, end = n["position"]/(job["bpm"]*8), (n["position"]+n["duration"])/(job["bpm"]*8)
        interior |= (times >= start+.08) & (times < end-.08)
    usable = interior & (frames[:, 2] > .5)
    error = np.abs(frames[usable, 0] - expected[usable])
    voiced_fraction = float(usable.sum()/max(1, interior.sum()))
    median = float(np.median(error)) if len(error) else 128.0
    p90 = float(np.quantile(error, .9)) if len(error) else 128.0
    aligned, shifts, ignore, reasons = align_boundaries(job, audio*gain, rate, frames)
    if voiced_fraction < .8: reasons.append("low_voicing_coverage")
    if median > .3 or p90 > .75: reasons.append("pitch_control_not_reproduced")
    if raw_clipped > .01 or raw_peak > 1.1: reasons.append("excessive_raw_clipping")
    qa = {"policy": QA_POLICY, "accepted": not reasons, "reasons": sorted(set(reasons)),
          "native_runner_sha256": digest(runner), "raw_audio_sha256": digest(raw),
          "voiced_fraction": voiced_fraction, "pitch_error_median_semitones": median,
          "pitch_error_p90_semitones": p90, "raw_clipped_fraction": raw_clipped,
          "raw_peak": raw_peak, "gain_db": float(20*np.log10(gain)), "aligned_boundaries": shifts,
          "ignore_intervals": ignore, "seconds": len(audio)/rate,
          "authority": "automatic_checks_only_not_human_review"}
    write_json(prefix.with_suffix(".qa.json"), qa)
    exported_notes = []
    for i, note in enumerate(aligned):
        phrase_start = i == 0 or aligned[i-1]["phrase"] != note["phrase"]
        flags = ["synthetic", "controlled_midi", "phrase_attack" if phrase_start else "legato"]
        if i and phrase_start and aligned[i-1]["pitch"] == note["pitch"]:
            flags.append("same_pitch_reattack")
        exported_notes.append({"id": note["id"], "start": note["start"], "end": note["end"],
                               "pitch": note["pitch"], "pitch_exact": float(note["pitch"]),
                               "gain_db": 0.0, "voiced_start": note["start"], "voiced_end": note["end"],
                               "lyric": record["vowel"], "flags": flags, "curve": []})
    waveform_sha = digest(final)
    annotation = {"version": 1, "audio": final.name, "sample_rate": rate, "duration": len(audio)/rate,
                  "bpm": job["bpm"], "notes": exported_notes,
                  "dataset": {"schema": "synthetic-obsidian-vocal-notes-1", "source_sha256": record["id"],
                              "audio_sha256": waveform_sha, "singer_id": record["bank"],
                              "session_id": f"synthetic-score-{record['score_index']:04}",
                              "review_status": "synthetic" if not reasons else "synthetic_rejected",
                              "time_unit": "seconds", "pitch_unit": "midi_semitones_a4_440",
                              "boundary_policy": "musical_notes; vibrato_and_slides_remain_in_curve"},
                  "training": {"label_origin": "controlled_synthesis_with_independent_acoustic_alignment",
                               "ignore_intervals": ignore}}
    annotation_path = prefix.with_suffix(".annotation.json")
    write_json(annotation_path, annotation)
    with prefix.with_suffix(".notes.csv").open("w") as stream:
        writer = csv.writer(stream)
        writer.writerow(["id", "start", "end", "pitch", "lyric"])
        writer.writerows([n[k] for k in ("id", "start", "end", "pitch", "lyric")] for n in exported_notes)
    prefix.with_suffix(".notes.txt").write_text("".join(f"{n['start']:.6f}\t{n['end']:.6f}\t{n['pitch']}\n" for n in exported_notes))
    synthesis = {"policy": QA_POLICY, "recipe": str(prefix.with_suffix(".recipe.json").relative_to(root)),
                 "recipe_sha256": record["recipe_sha256"], "midi": str(prefix.with_suffix(".mid").relative_to(root)),
                 "midi_sha256": record["midi_sha256"], "qa": str(prefix.with_suffix(".qa.json").relative_to(root)),
                 "qa_sha256": digest(prefix.with_suffix(".qa.json")), "plan": "plan.json",
                 "plan_sha256": digest(root/"plan.json"), "bank": record["bank"], "vowel": record["vowel"],
                 "category": record["category"], "score_index": record["score_index"]}
    return {"id": record["id"], "audio": str(final.relative_to(root)), "audio_sha256": waveform_sha,
            "annotation": str(annotation_path.relative_to(root)), "annotation_sha256": digest(annotation_path),
            "seconds": len(audio)/rate, "sample_rate": rate, "split": "train",
            "group": f"synthetic-score:{record['score_index']:04}", "notes": exported_notes,
            "singer_id": record["bank"], "synthesis": synthesis, "ignore_intervals": ignore,
            "review_authority": "controlled_synthesis_not_human_reviewed"}, qa


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dataset", type=Path, required=True)
    parser.add_argument("--native-runner", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--limit", type=int)
    args = parser.parse_args()
    root, runner = args.dataset.resolve(), args.native_runner.resolve()
    plan = json.loads((root/"plan.json").read_text())
    if plan["schema"] != SCHEMA or digest(root/"jobs.json") != plan["jobs_sha256"]:
        raise ValueError("Synthesis plan changed")
    jobs = {j["id"]: j for j in json.loads((root/"jobs.json").read_text())}
    source = plan["records"][:args.limit] if args.limit else plan["records"]
    accepted, rejected, checks = [], [], []
    def check(record):
        return process(root, record, jobs[record["id"]], runner)
    with ThreadPoolExecutor(max_workers=args.workers) as pool:
        for count, (record, qa) in enumerate(pool.map(check, source), 1):
            (accepted if qa["accepted"] else rejected).append(record)
            checks.append(qa)
            if count % 32 == 0 or count == len(source):
                print(json.dumps({"checked": count, "accepted": len(accepted), "rejected": len(rejected)}), flush=True)
    document = {"schema": SCHEMA, "qa_policy": QA_POLICY, "plan": "plan.json",
                "plan_sha256": digest(root/"plan.json"), "records": accepted}
    suffix = "-pilot" if args.limit else ""
    write_json(root/f"manifest{suffix}.json", document)
    write_json(root/f"rejected{suffix}.json", {**document, "records": rejected})
    summary = {"planned": len(source), "accepted": len(accepted), "rejected": len(rejected),
               "seconds": sum(r["seconds"] for r in accepted), "notes": sum(len(r["notes"]) for r in accepted),
               "by_bank": dict(Counter(r["synthesis"]["bank"] for r in accepted)),
               "by_vowel": dict(Counter(r["synthesis"]["vowel"] for r in accepted)),
               "by_category": dict(Counter(r["synthesis"]["category"] for r in accepted)),
               "legato_boundaries": sum("legato" in n["flags"] for r in accepted for n in r["notes"]),
               "phrase_attacks": sum("phrase_attack" in n["flags"] for r in accepted for n in r["notes"]),
               "same_pitch_reattacks": sum("same_pitch_reattack" in n["flags"] for r in accepted for n in r["notes"]),
               "ignored_seconds": sum(end-start for r in accepted for start,end in r["ignore_intervals"]),
               "rejection_reasons": dict(Counter(reason for q in checks if not q["accepted"] for reason in q["reasons"])),
               "median_pitch_error_semitones": float(np.median([q["pitch_error_median_semitones"] for q in checks])),
               "scope": "Automatic synthesis/QA only. Not human reviewed. No boundary model used to label."}
    write_json(root/f"summary{suffix}.json", summary)
    print(json.dumps(summary, indent=2), flush=True)


if __name__ == "__main__":
    main()
