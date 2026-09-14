#!/usr/bin/env python3
"""Persistent compact backing-vocal worker for the Vocal Annotation Tool.

The JUCE app starts this helper once and communicates through a small file queue:

- app writes `<request_id>.request.json`
- worker appends JSON events to `<request_id>.events.jsonl`
- worker deletes the request when it has accepted it

This keeps the MLX Transformer loaded across Add BV requests and allows the UI
to draw notes after every generated vocal window.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import traceback
from pathlib import Path
from typing import Any

from generate_backing_vocals_transformer import (
    DEFAULT_TRANSFORMER,
    generate_with_transformer,
    load_transformer,
    supports_style,
)
from generate_backing_vocals_openai import generate_with_openai


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--queue-dir", type=Path, required=True)
    parser.add_argument("--transformer-path", type=Path, default=DEFAULT_TRANSFORMER)
    parser.add_argument("--max-lead-notes", type=int, default=48)
    parser.add_argument("--max-gap-beats", type=float, default=2.0)
    parser.add_argument("--poll-ms", type=int, default=80)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    queue_dir = args.queue_dir.expanduser().resolve()
    queue_dir.mkdir(parents=True, exist_ok=True)

    transformer_path = args.transformer_path.expanduser()
    write_status(queue_dir, {"type": "loading", "engine": "compact_transformer", "model_path": str(transformer_path)})
    transformer_model, transformer_config = load_transformer(transformer_path)
    state = {
        "transformer_model": transformer_model,
        "transformer_config": transformer_config,
    }
    write_status(queue_dir, {"type": "ready", "engine": "compact_transformer", "model_path": str(transformer_path)})

    while True:
        requests = sorted(queue_dir.glob("*.request.json"), key=lambda path: path.stat().st_mtime)
        if not requests:
            time.sleep(max(0.01, args.poll_ms / 1000.0))
            continue

        for request_path in requests:
            process_request(request_path, args, state)


def process_request(request_path: Path, args: argparse.Namespace, state: dict[str, Any]) -> None:
    request_id = request_path.name.removesuffix(".request.json")
    processing_path = request_path.with_name(f"{request_id}.processing.json")
    events_path = request_path.with_name(f"{request_id}.events.jsonl")

    try:
        request_path.rename(processing_path)
    except FileNotFoundError:
        return
    except OSError:
        # The app may still be finishing the atomic rename. Try again on next poll.
        return

    def emit(event: dict[str, Any]) -> None:
        event = {"request_id": request_id, **event}
        with events_path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps(event, ensure_ascii=False) + "\n")
            handle.flush()

    try:
        request = json.loads(processing_path.read_text(encoding="utf-8"))
        style_id = str(request.get("style_id", ""))
        backend = str(request.get("generation_backend") or "local")
        model = str(request.get("generation_model") or "")
        emit(
            {
                "type": "started",
                "engine": "openai_responses" if backend == "openai" else "compact_transformer",
                "model": model,
            }
        )
        if backend == "openai":
            result = generate_with_openai(
                request,
                args,
                model or "gpt-5.6-luna",
                stream_callback=emit,
            )
        else:
            if not supports_style(state["transformer_config"], style_id):
                raise ValueError(f"Compact Transformer does not support style {style_id}")
            result = generate_with_transformer(
                request,
                args,
                state["transformer_model"],
                state["transformer_config"],
                stream_callback=emit,
            )
        result_path = processing_path.with_name(f"{request_id}.result.json")
        result_path.write_text(json.dumps(result, ensure_ascii=False), encoding="utf-8")
        emit(
            {
                "type": "done",
                "style_id": result.get("style_id"),
                "style_name": result.get("style_name"),
                "engine": result.get("engine"),
                "model": result.get("model", ""),
                "window_count": result.get("window_count"),
                "note_count": result.get("note_count"),
                "warnings": result.get("warnings", []),
                "debug_dsl_path": result.get("debug_dsl_path", ""),
                "result_path": str(result_path),
            }
        )
    except Exception as exc:  # noqa: BLE001 - worker must keep serving following requests.
        emit(
            {
                "type": "error",
                "message": str(exc),
                "traceback": traceback.format_exc(limit=8),
            }
        )
    finally:
        try:
            processing_path.unlink()
        except FileNotFoundError:
            pass


def write_status(queue_dir: Path, payload: dict[str, Any]) -> None:
    status_path = queue_dir / "worker_status.json"
    status_path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    print(json.dumps(payload, ensure_ascii=False), flush=True, file=sys.stderr)


if __name__ == "__main__":
    main()
