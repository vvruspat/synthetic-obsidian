#!/usr/bin/env python3
"""Prepare, submit, and apply AI-generated backing vocals for DSL YAML files."""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

from generate_backing_vocals_dsl import insert_backing, parse_song, render_backing_yaml


API_BASE = "https://api.openai.com/v1"
DEFAULT_MODEL = "gpt-5.5"
SYSTEM_PROMPT = """You are a professional vocal arranger and music-theory expert.
Generate musical backing-vocal parts for a MIDI-derived song DSL.
Do not rewrite the lead vocal or chords. Return only valid JSON that matches the requested schema.
Prefer beautiful, singable, phrase-aware backing vocals over mechanical transposition."""

USER_PROMPT = """Create one high-quality AI backing-vocal arrangement set for this song.

Musical requirements:
- Use classical voice leading, harmony, counterpoint, and modern pop/rock/gospel practice where fitting.
- Preserve the lead rhythm where it is musically natural, but allow rests, held tones, suspensions, passing tones, oblique motion, and contrary motion.
- Use 1 to 3 backing parts when useful. Avoid crowding. Keep every part singable.
- Each note must reference an existing lead note through source_lead_ref and an existing chord through chord_ref.
- Keep starts/durations in beats. Use the same phrase_id/chord_ref/source_lead_ref values from source lead notes.
- Do not output drums, bass, piano, orchestration, effects, MIDI CC, pitch bend, or sustain.
- Do not include markdown.

Return compact JSON exactly in this shape:
{
  "backing_vocals_compact": [
    {
      "id": "AI_GPT_ARRANGER",
      "name": "AI GPT Arranger",
      "description": "Phrase-aware AI backing-vocal arrangement.",
      "confidence": 0.0,
      "note_schema": ["id","start","duration","bar","beat","pitch","midi_note","velocity","syllable","phrase_id","chord_ref","source_lead_ref"],
      "parts": [
        {
          "id": "ai_upper_part_01",
          "role": "soprano|alto|tenor|bass|general",
          "strategy": "ai_arranged",
          "range": "C3-C5",
          "notes": [
            ["ai_upper_001",0.0,0.5,1,1,"C5",72,70,null,"phrase_001","chord_001","lead_001"]
          ]
        }
      ]
    }
  ]
}

Song JSON:
The song uses compact arrays; read chords_schema and lead_vocal_schema to map each array column.
"""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    prepare = subparsers.add_parser("prepare", help="Build a Batch API JSONL request file.")
    prepare.add_argument("--dsl-root", type=Path, required=True)
    prepare.add_argument("--output-dir", type=Path, required=True)
    prepare.add_argument("--model", default=os.environ.get("OPENAI_MODEL", DEFAULT_MODEL))
    prepare.add_argument("--limit", type=int)
    prepare.add_argument("--max-lead-notes", type=int, default=0, help="0 means send the full lead track.")
    prepare.add_argument("--max-chords", type=int, default=0, help="0 means send the full chord track.")
    prepare.add_argument("--max-output-tokens", type=int, default=12000)
    prepare.add_argument("--shard-max-bytes", type=int, default=0, help="Also write requests.partNNN.jsonl files below this byte budget.")
    prepare.add_argument("--shard-max-requests", type=int, default=0, help="Also start a new shard after this many requests.")
    prepare.add_argument("--overwrite", action="store_true")

    submit = subparsers.add_parser("submit", help="Upload the JSONL and create a 24h batch.")
    submit.add_argument("--requests-jsonl", type=Path, required=True)

    status = subparsers.add_parser("status", help="Fetch batch status.")
    status.add_argument("--batch-id", required=True)

    download = subparsers.add_parser("download", help="Download a completed batch output file.")
    download.add_argument("--batch-id", required=True)
    download.add_argument("--output-dir", type=Path, required=True)

    run_shards = subparsers.add_parser("run-shards", help="Submit shard JSONL files sequentially and download completed outputs.")
    run_shards.add_argument("--shard-dir", type=Path, required=True)
    run_shards.add_argument("--poll-seconds", type=int, default=300)
    run_shards.add_argument("--start-at", type=int, default=1)
    run_shards.add_argument("--stop-after", type=int, default=0, help="0 means run all remaining shards.")

    apply = subparsers.add_parser("apply", help="Apply batch results to a new DSL directory.")
    apply.add_argument("--results-jsonl", type=Path, required=True)
    apply.add_argument("--manifest", type=Path, required=True)
    apply.add_argument("--dsl-root", type=Path, required=True)
    apply.add_argument("--output-root", type=Path, required=True)
    apply.add_argument("--overwrite", action="store_true")

    return parser.parse_args()


def command_prepare(args: argparse.Namespace) -> None:
    dsl_root = args.dsl_root.expanduser().resolve()
    output_dir = args.output_dir.expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    requests_path = output_dir / "requests.jsonl"
    manifest_path = output_dir / "manifest.json"
    if (requests_path.exists() or manifest_path.exists()) and not args.overwrite:
        raise SystemExit(f"{output_dir} already contains batch files; pass --overwrite")

    rows: list[dict[str, str]] = []
    written = 0
    with requests_path.open("w", encoding="utf-8") as request_file:
        for source in sorted(dsl_root.rglob("*.yaml")):
            if args.limit is not None and written >= args.limit:
                break
            text = source.read_text(encoding="utf-8")
            song = parse_song(text)
            if not song.chords or not song.lead:
                continue
            custom_id = f"dsl-{written + 1:05d}-{slug(source.relative_to(dsl_root).as_posix())}"
            payload = build_song_payload(song, args.max_lead_notes, args.max_chords)
            body = {
                "model": args.model,
                "input": [
                    {"role": "system", "content": SYSTEM_PROMPT},
                    {"role": "user", "content": USER_PROMPT + json.dumps(payload, ensure_ascii=False, separators=(",", ":"))},
                ],
                "text": {"format": {"type": "json_object"}},
                "max_output_tokens": args.max_output_tokens,
            }
            request = {"custom_id": custom_id, "method": "POST", "url": "/v1/responses", "body": body}
            request_file.write(json.dumps(request, ensure_ascii=False, separators=(",", ":")) + "\n")
            rows.append({"custom_id": custom_id, "path": source.relative_to(dsl_root).as_posix()})
            written += 1

    manifest_path.write_text(json.dumps({"dsl_root": str(dsl_root), "model": args.model, "files": rows}, indent=2), encoding="utf-8")
    if args.shard_max_bytes > 0 or args.shard_max_requests > 0:
        shard_count = write_shards(requests_path, output_dir, args.shard_max_bytes, args.shard_max_requests)
        print(f"Wrote {shard_count} shard files")
    print(f"Wrote {requests_path}")
    print(f"Wrote {manifest_path}")
    print(f"Prepared {written} requests")


def write_shards(requests_path: Path, output_dir: Path, max_bytes: int, max_requests: int) -> int:
    shard_index = 1
    current_size = 0
    current_requests = 0
    current_file = None
    try:
        for line in requests_path.open("rb"):
            over_bytes = max_bytes > 0 and current_size > 0 and current_size + len(line) > max_bytes
            over_requests = max_requests > 0 and current_requests >= max_requests
            if current_file is None or over_bytes or over_requests:
                if current_file is not None:
                    current_file.close()
                current_file = (output_dir / f"requests.part{shard_index:03d}.jsonl").open("wb")
                shard_index += 1
                current_size = 0
                current_requests = 0
            current_file.write(line)
            current_size += len(line)
            current_requests += 1
    finally:
        if current_file is not None:
            current_file.close()
    return shard_index - 1


def build_song_payload(song: Any, max_lead_notes: int, max_chords: int) -> dict[str, Any]:
    lead = list(song.lead if max_lead_notes <= 0 else song.lead[:max_lead_notes])
    chords = list(song.chords if max_chords <= 0 else song.chords[:max_chords])
    return {
        "meta": song.meta,
        "limits": {
            "lead_notes_sent": len(lead),
            "lead_notes_total": len(song.lead),
            "chords_sent": len(chords),
            "chords_total": len(song.chords),
            "must_only_reference_sent_lead_notes": len(lead) < len(song.lead),
        },
        "chords_schema": ["id", "start", "duration", "bar", "beat", "chord", "root", "quality", "bass", "notes"],
        "chords": [
            [chord.id, chord.start, chord.duration, chord.bar, chord.beat, chord.chord, chord.root, chord.quality, chord.bass, list(chord.notes)]
            for chord in chords
        ],
        "lead_vocal_schema": ["id", "start", "duration", "bar", "beat", "pitch", "midi_note", "velocity", "syllable", "phrase_id", "chord_ref"],
        "lead_vocal": [
            [note.id, note.start, note.duration, note.bar, note.beat, note.pitch, note.midi_note, note.velocity, note.syllable, note.phrase_id, note.chord_ref]
            for note in lead
        ],
    }


def command_submit(args: argparse.Namespace) -> None:
    requests_path = args.requests_jsonl.expanduser().resolve()
    upload = upload_file(requests_path)
    batch = api_json(
        "POST",
        "/batches",
        {
            "input_file_id": upload["id"],
            "endpoint": "/v1/responses",
            "completion_window": "24h",
            "metadata": {"description": "Synthetic Obsidian AI backing vocals"},
        },
    )
    print(json.dumps(batch, indent=2, ensure_ascii=False))


def command_status(args: argparse.Namespace) -> None:
    print(json.dumps(api_json("GET", f"/batches/{args.batch_id}"), indent=2, ensure_ascii=False))


def command_download(args: argparse.Namespace) -> None:
    output_dir = args.output_dir.expanduser().resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    batch = api_json("GET", f"/batches/{args.batch_id}")
    (output_dir / "batch.json").write_text(json.dumps(batch, indent=2, ensure_ascii=False), encoding="utf-8")
    if not batch.get("output_file_id"):
        raise SystemExit(f"Batch is not ready; status={batch.get('status')}")
    content = api_bytes("GET", f"/files/{batch['output_file_id']}/content")
    destination = output_dir / "results.jsonl"
    destination.write_bytes(content)
    print(f"Wrote {destination}")


def command_run_shards(args: argparse.Namespace) -> None:
    shard_dir = args.shard_dir.expanduser().resolve()
    state_path = shard_dir / "shard_runs.json"
    state = load_state(state_path)
    shards = sorted(shard_dir.glob("requests.part*.jsonl"))
    selected = [path for path in shards if shard_number(path) >= args.start_at]
    if args.stop_after > 0:
        selected = selected[: args.stop_after]
    if not selected:
        raise SystemExit(f"No shard files found in {shard_dir}")

    for shard in selected:
        shard_name = shard.name
        record = state.setdefault(shard_name, {})
        if record.get("status") == "completed" and record.get("output_path"):
            print(f"{shard_name}: already completed")
            continue
        if not record.get("batch_id"):
            print(f"{shard_name}: submitting")
            upload = upload_file(shard)
            batch = api_json(
                "POST",
                "/batches",
                {
                    "input_file_id": upload["id"],
                    "endpoint": "/v1/responses",
                    "completion_window": "24h",
                    "metadata": {"description": f"Synthetic Obsidian AI backing vocals {shard_name}"},
                },
            )
            record.update({"batch_id": batch["id"], "input_file_id": upload["id"], "status": batch["status"]})
            save_state(state_path, state)
            print(f"{shard_name}: batch {batch['id']} {batch['status']}")

        batch_id = record["batch_id"]
        while True:
            batch = api_json("GET", f"/batches/{batch_id}")
            record["status"] = batch["status"]
            record["request_counts"] = batch.get("request_counts", {})
            record["usage"] = batch.get("usage", {})
            save_state(state_path, state)
            counts = batch.get("request_counts", {})
            print(
                f"{shard_name}: {batch['status']} "
                f"completed={counts.get('completed', 0)} failed={counts.get('failed', 0)} total={counts.get('total', 0)}"
            )
            if batch["status"] == "completed":
                if batch.get("output_file_id"):
                    destination = shard_dir / shard.name.replace("requests.", "results.")
                    destination.write_bytes(api_bytes("GET", f"/files/{batch['output_file_id']}/content"))
                    record["output_file_id"] = batch["output_file_id"]
                    record["output_path"] = destination.name
                    save_state(state_path, state)
                    print(f"{shard_name}: wrote {destination}")
                break
            if batch["status"] in {"failed", "expired", "cancelled"}:
                record["errors"] = batch.get("errors")
                save_state(state_path, state)
                raise SystemExit(f"{shard_name}: batch ended with status {batch['status']}")
            time.sleep(max(30, args.poll_seconds))


def command_apply(args: argparse.Namespace) -> None:
    manifest = json.loads(args.manifest.expanduser().resolve().read_text(encoding="utf-8"))
    mapping = {row["custom_id"]: row["path"] for row in manifest["files"]}
    dsl_root = args.dsl_root.expanduser().resolve()
    output_root = args.output_root.expanduser().resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    applied = 0
    skipped = 0
    for line_number, line in enumerate(args.results_jsonl.expanduser().resolve().read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip():
            continue
        row = json.loads(line)
        custom_id = row.get("custom_id")
        relative = mapping.get(custom_id)
        if not relative:
            print(f"skip line {line_number}: unknown custom_id {custom_id}", file=sys.stderr)
            skipped += 1
            continue
        if row.get("error"):
            print(f"skip {relative}: {row['error']}", file=sys.stderr)
            skipped += 1
            continue
        try:
            backing = extract_backing(row)
        except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
            print(f"skip {relative}: invalid AI response: {error}", file=sys.stderr)
            skipped += 1
            continue
        if not backing:
            print(f"skip {relative}: no valid backing_vocals JSON", file=sys.stderr)
            skipped += 1
            continue
        source = dsl_root / relative
        destination = output_root / relative
        if destination.exists() and not args.overwrite:
            print(f"skip {relative}: destination exists", file=sys.stderr)
            skipped += 1
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        source_text = source.read_text(encoding="utf-8")
        destination.write_text(insert_backing(source_text, render_backing_yaml(backing)), encoding="utf-8")
        applied += 1

    print(f"Applied {applied} files to {output_root}")
    if skipped:
        print(f"Skipped {skipped} rows")


def load_state(path: Path) -> dict[str, Any]:
    if not path.exists():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))


def save_state(path: Path, state: dict[str, Any]) -> None:
    path.write_text(json.dumps(state, indent=2, ensure_ascii=False), encoding="utf-8")


def shard_number(path: Path) -> int:
    match = re.search(r"part(\d+)", path.name)
    return int(match.group(1)) if match else 0



def extract_backing(batch_row: dict[str, Any]) -> list[dict[str, Any]]:
    body = batch_row.get("response", {}).get("body", {})
    if body.get("status") == "incomplete":
        details = body.get("incomplete_details") or {}
        raise ValueError(f"incomplete response: {details.get('reason', 'unknown')}")
    text = body.get("output_text")
    if not isinstance(text, str):
        chunks: list[str] = []
        for output in body.get("output", []):
            for content in output.get("content", []):
                if content.get("type") in {"output_text", "text"} and isinstance(content.get("text"), str):
                    chunks.append(content["text"])
        text = "\n".join(chunks)
    if not isinstance(text, str) or not text.strip():
        return []
    payload = json.loads(strip_json_fences(text))
    if not isinstance(payload, dict):
        return []
    compact = payload.get("backing_vocals_compact")
    if isinstance(compact, list):
        return normalize_backing(expand_compact_backing(compact))
    backing = payload.get("backing_vocals")
    return normalize_backing(backing if isinstance(backing, list) else [])


def expand_compact_backing(compact_tracks: list[dict[str, Any]]) -> list[dict[str, Any]]:
    expanded_tracks: list[dict[str, Any]] = []
    default_schema = ["id", "start", "duration", "bar", "beat", "pitch", "midi_note", "velocity", "syllable", "phrase_id", "chord_ref", "source_lead_ref"]
    for track in compact_tracks:
        if not isinstance(track, dict):
            continue
        schema = track.get("note_schema")
        note_schema = schema if isinstance(schema, list) and all(isinstance(item, str) for item in schema) else default_schema
        expanded_track = {key: value for key, value in track.items() if key != "note_schema"}
        expanded_parts: list[dict[str, Any]] = []
        for part in track.get("parts", []):
            if not isinstance(part, dict):
                continue
            expanded_part = dict(part)
            notes: list[dict[str, Any]] = []
            for note in part.get("notes", []):
                if isinstance(note, list):
                    notes.append({key: note[index] for index, key in enumerate(note_schema) if index < len(note)})
                elif isinstance(note, dict):
                    notes.append(note)
            expanded_part["notes"] = notes
            expanded_parts.append(expanded_part)
        expanded_track["parts"] = expanded_parts
        expanded_tracks.append(expanded_track)
    return expanded_tracks


def normalize_backing(backing: list[dict[str, Any]]) -> list[dict[str, Any]]:
    required_note_keys = {
        "start",
        "duration",
        "bar",
        "beat",
        "pitch",
        "midi_note",
        "velocity",
        "phrase_id",
        "chord_ref",
        "source_lead_ref",
    }
    normalized_tracks: list[dict[str, Any]] = []
    for track_index, track in enumerate(backing, start=1):
        if not isinstance(track, dict):
            continue
        track.setdefault("id", f"AI_GPT_ARRANGER_{track_index:02d}")
        track.setdefault("name", "AI GPT Arranger")
        track.setdefault("description", "Phrase-aware AI backing-vocal arrangement.")
        track.setdefault("confidence", 0.82)
        normalized_parts: list[dict[str, Any]] = []
        for part_index, part in enumerate(track.get("parts", []), start=1):
            if not isinstance(part, dict):
                continue
            part.setdefault("id", f"ai_part_{part_index:02d}")
            part.setdefault("role", "general")
            part.setdefault("strategy", "ai_arranged")
            part.setdefault("range", "C3-C5")
            valid_notes: list[dict[str, Any]] = []
            for note_index, note in enumerate(part.get("notes", []), start=1):
                if not isinstance(note, dict) or not required_note_keys.issubset(note):
                    continue
                note.setdefault("id", f"{part['id']}_note_{note_index:03d}")
                note.setdefault("syllable", None)
                valid_notes.append(note)
            if valid_notes:
                part["notes"] = valid_notes
                normalized_parts.append(part)
        if normalized_parts:
            track["parts"] = normalized_parts
            normalized_tracks.append(track)
    return normalized_tracks


def slug(value: str) -> str:
    value = re.sub(r"[^A-Za-z0-9]+", "-", value).strip("-").lower()
    return value[:80] or "song"


def strip_json_fences(text: str) -> str:
    text = text.strip()
    if text.startswith("```"):
        text = re.sub(r"^```(?:json)?\s*", "", text)
        text = re.sub(r"\s*```$", "", text)
    return text.strip()


def require_api_key() -> str:
    api_key = os.environ.get("OPENAI_API_KEY")
    if not api_key:
        raise SystemExit("OPENAI_API_KEY is not set")
    return api_key


def upload_file(path: Path) -> dict[str, Any]:
    boundary = "----synthetic-obsidian-batch"
    body = b"".join(
        [
            f"--{boundary}\r\n".encode(),
            b'Content-Disposition: form-data; name="purpose"\r\n\r\nbatch\r\n',
            f"--{boundary}\r\n".encode(),
            f'Content-Disposition: form-data; name="file"; filename="{path.name}"\r\n'.encode(),
            b"Content-Type: application/jsonl\r\n\r\n",
            path.read_bytes(),
            f"\r\n--{boundary}--\r\n".encode(),
        ]
    )
    return json.loads(api_bytes("POST", "/files", body, f"multipart/form-data; boundary={boundary}").decode("utf-8"))


def api_json(method: str, path: str, payload: dict[str, Any] | None = None) -> dict[str, Any]:
    data = json.dumps(payload).encode("utf-8") if payload is not None else None
    return json.loads(api_bytes(method, path, data, "application/json").decode("utf-8"))


def api_bytes(method: str, path: str, data: bytes | None = None, content_type: str | None = None) -> bytes:
    request = urllib.request.Request(f"{API_BASE}{path}", data=data, method=method)
    request.add_header("Authorization", f"Bearer {require_api_key()}")
    if content_type:
        request.add_header("Content-Type", content_type)
    try:
        with urllib.request.urlopen(request, timeout=120) as response:
            return response.read()
    except urllib.error.HTTPError as error:
        detail = error.read().decode("utf-8", errors="replace")
        raise SystemExit(f"OpenAI API error {error.code}: {detail}") from error


def main() -> None:
    args = parse_args()
    if args.command == "prepare":
        command_prepare(args)
    elif args.command == "submit":
        command_submit(args)
    elif args.command == "status":
        command_status(args)
    elif args.command == "download":
        command_download(args)
    elif args.command == "run-shards":
        command_run_shards(args)
    elif args.command == "apply":
        command_apply(args)


if __name__ == "__main__":
    main()
