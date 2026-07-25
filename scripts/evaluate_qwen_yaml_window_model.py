#!/usr/bin/env python3
"""Evaluate a Qwen LoRA adapter on native YAML vocal-window examples."""

from __future__ import annotations

import argparse
import json
import random
import re
import subprocess
import sys
from pathlib import Path
from typing import Any

import yaml


DEFAULT_SYSTEM = (
    "You generate Synthetic Obsidian backing-vocal YAML DSL. "
    "Return only valid YAML containing tracks.backing_vocals. "
    "Do not include markdown, analysis, chords, lead_vocal, or explanations."
)

NOTE_TO_PC = {
    "C": 0,
    "C#": 1,
    "Db": 1,
    "D": 2,
    "D#": 3,
    "Eb": 3,
    "E": 4,
    "F": 5,
    "F#": 6,
    "Gb": 6,
    "G": 7,
    "G#": 8,
    "Ab": 8,
    "A": 9,
    "A#": 10,
    "Bb": 10,
    "B": 11,
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", default="Qwen/Qwen3-0.6B")
    parser.add_argument("--adapter-path", type=Path, required=True)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=10)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--max-tokens", type=int, default=2200)
    parser.add_argument("--output", type=Path)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    rows = load_sample(args.data.expanduser().resolve(), args.limit, random.Random(args.seed))
    results = []
    for index, row in enumerate(rows, start=1):
        print(f"[{index}/{len(rows)}] {row.get('metadata', {}).get('style_id', '')}", flush=True)
        result = evaluate_row(row, args)
        results.append(result)
        print(json.dumps(summarize_result(result), ensure_ascii=False), flush=True)

    summary = {
        "examples": len(results),
        "yaml_ok": sum(result["yaml_ok"] for result in results),
        "valid": sum(result["valid"] for result in results),
        "total_errors": sum(len(result["errors"]) for result in results),
        "total_warnings": sum(len(result["warnings"]) for result in results),
    }
    payload = {"summary": summary, "results": results}
    if args.output:
        args.output.expanduser().resolve().write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(summary, ensure_ascii=False, indent=2))


def load_sample(path: Path, limit: int, rng: random.Random) -> list[dict[str, Any]]:
    rows = []
    with path.open(encoding="utf-8") as handle:
        for line in handle:
            if line.strip():
                rows.append(json.loads(line))
    rng.shuffle(rows)
    return rows[:limit]


def evaluate_row(row: dict[str, Any], args: argparse.Namespace) -> dict[str, Any]:
    messages = row["messages"]
    system = messages[0]["content"] if messages and messages[0]["role"] == "system" else DEFAULT_SYSTEM
    prompt = next(message["content"] for message in messages if message["role"] == "user")
    generated = run_generate(args.model, args.adapter_path, system, prompt, args.max_tokens)
    generated = clean_generation(generated)
    source_yaml = prompt.split("\n\n", 1)[1]
    return validate_output(row.get("metadata", {}), source_yaml, generated)


def run_generate(model: str, adapter_path: Path, system: str, prompt: str, max_tokens: int) -> str:
    command = [
        sys.executable,
        "-m",
        "mlx_lm",
        "generate",
        "--model",
        model,
        "--adapter-path",
        str(adapter_path),
        "--system-prompt",
        system,
        "--prompt",
        "-",
        "--chat-template-config",
        '{"enable_thinking":false}',
        "--max-tokens",
        str(max_tokens),
        "--temp",
        "0.0",
        "--verbose",
        "False",
    ]
    completed = subprocess.run(command, input=prompt, text=True, capture_output=True, check=False)
    if completed.returncode != 0:
        return completed.stdout + "\n" + completed.stderr
    return completed.stdout


def clean_generation(text: str) -> str:
    lines = []
    for line in text.splitlines():
        if line.startswith("Fetching "):
            continue
        if line.startswith("\rFetching "):
            continue
        if line.startswith("<think>") or line.startswith("</think>"):
            continue
        lines.append(line)
    return "\n".join(lines).strip()


def validate_output(metadata: dict[str, Any], source_text: str, generated: str) -> dict[str, Any]:
    errors: list[str] = []
    warnings: list[str] = []
    source = safe_yaml_load(source_text, errors, "source")
    output = safe_yaml_load(generated, errors, "generated")
    lead_by_id = {}
    if isinstance(source, dict):
        for note in source.get("tracks", {}).get("lead_vocal", []) or []:
            if isinstance(note, dict):
                lead_by_id[str(note.get("id", ""))] = note
    backing = []
    if isinstance(output, dict):
        backing = output.get("tracks", {}).get("backing_vocals", []) or []
    if len(backing) != 1:
        errors.append(f"expected 1 backing set, got {len(backing)}")

    note_count = 0
    midi_mismatches = 0
    timing_warnings = 0
    missing_refs = 0
    style_id = metadata.get("style_id")
    if backing and isinstance(backing[0], dict):
        if style_id and backing[0].get("id") != style_id:
            warnings.append(f"style id mismatch: expected {style_id}, got {backing[0].get('id')}")
        parts = backing[0].get("parts", []) or []
        if not parts:
            errors.append("no parts")
        for part in parts:
            notes = part.get("notes", []) if isinstance(part, dict) else []
            if not notes:
                warnings.append(f"part has no notes: {part.get('id') if isinstance(part, dict) else ''}")
            for note in notes:
                note_count += 1
                if not isinstance(note, dict):
                    errors.append("note is not a mapping")
                    continue
                source_ref = str(note.get("source_lead_ref", ""))
                lead_note = lead_by_id.get(source_ref)
                if not lead_note:
                    missing_refs += 1
                    continue
                if "pitch" in note and "midi_note" in note:
                    expected_midi = pitch_to_midi(str(note["pitch"]))
                    if expected_midi is not None and int(note["midi_note"]) != expected_midi:
                        midi_mismatches += 1
                if not is_pedal_or_drone(str(part.get("strategy", ""))):
                    if abs(float(note.get("start", 0)) - float(lead_note.get("start", 0))) > 0.01:
                        timing_warnings += 1
                    if abs(float(note.get("duration", 0)) - float(lead_note.get("duration", 0))) > 0.01:
                        timing_warnings += 1
    if note_count == 0:
        errors.append("no notes")
    if missing_refs:
        errors.append(f"missing source_lead_ref count: {missing_refs}")
    if midi_mismatches:
        warnings.append(f"midi_note mismatches: {midi_mismatches}")
    if timing_warnings:
        warnings.append(f"timing warnings: {timing_warnings}")

    return {
        "metadata": metadata,
        "yaml_ok": not any(error.startswith("generated yaml parse failed") for error in errors),
        "valid": not errors,
        "errors": errors,
        "warnings": warnings,
        "note_count": note_count,
        "generated": generated,
    }


def safe_yaml_load(text: str, errors: list[str], label: str) -> Any:
    try:
        return yaml.safe_load(text)
    except Exception as exc:
        errors.append(f"{label} yaml parse failed: {exc}")
        return None


def pitch_to_midi(pitch: str) -> int | None:
    match = re.fullmatch(r"([A-G](?:#|b)?)(-?\d+)", pitch)
    if not match:
        return None
    note, octave = match.groups()
    return (int(octave) + 1) * 12 + NOTE_TO_PC[note]


def is_pedal_or_drone(strategy: str) -> bool:
    return "drone" in strategy or "pedal" in strategy


def summarize_result(result: dict[str, Any]) -> dict[str, Any]:
    return {
        "source_file": result["metadata"].get("source_file"),
        "style_id": result["metadata"].get("style_id"),
        "yaml_ok": result["yaml_ok"],
        "valid": result["valid"],
        "errors": result["errors"],
        "warnings": result["warnings"],
        "note_count": result["note_count"],
    }


if __name__ == "__main__":
    main()
