#!/usr/bin/env python3
"""Persistent SoulX-Singer-SVC worker for offline backing-audio renders."""

from __future__ import annotations

import argparse
import json
import sys
import time
import traceback
from pathlib import Path
from typing import Any

from render_backing_vocal_soulx import (
    DEFAULT_CONFIG_PATH,
    DEFAULT_MODEL_PATH,
    DEFAULT_SOURCE_ROOT,
    SoulXRenderer,
)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--queue-dir", type=Path, required=True)
    parser.add_argument("--source-root", type=Path, default=DEFAULT_SOURCE_ROOT)
    parser.add_argument("--model", type=Path, default=DEFAULT_MODEL_PATH)
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG_PATH)
    parser.add_argument("--device", default="auto")
    parser.add_argument("--steps", type=int, default=16)
    parser.add_argument("--poll-ms", type=int, default=80)
    return parser.parse_args()


def write_status(queue_dir: Path, payload: dict[str, Any]) -> None:
    (queue_dir / "worker_status.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )
    print(json.dumps(payload, ensure_ascii=False), flush=True, file=sys.stderr)


def main() -> None:
    args = parse_args()
    queue_dir = args.queue_dir.expanduser().resolve()
    queue_dir.mkdir(parents=True, exist_ok=True)

    write_status(queue_dir, {"type": "loading", "engine": "SoulX-Singer-SVC"})
    try:
        renderer = SoulXRenderer(
            source_root=args.source_root,
            model_path=args.model,
            config_path=args.config,
            device=args.device,
            steps=args.steps,
        )
    except Exception as exc:  # noqa: BLE001 - report startup failures to the app.
        write_status(
            queue_dir,
            {
                "type": "error",
                "engine": "SoulX-Singer-SVC",
                "message": str(exc),
                "traceback": traceback.format_exc(limit=12),
            },
        )
        return
    write_status(
        queue_dir,
        {"type": "ready", "engine": "SoulX-Singer-SVC", "device": renderer.device},
    )

    while True:
        requests = sorted(queue_dir.glob("*.request.json"), key=lambda path: path.stat().st_mtime)
        if not requests:
            time.sleep(max(0.01, args.poll_ms / 1000.0))
            continue
        for request_path in requests:
            process_request(request_path, renderer)


def process_request(request_path: Path, renderer: SoulXRenderer) -> None:
    request_id = request_path.name.removesuffix(".request.json")
    processing_path = request_path.with_name(f"{request_id}.processing.json")
    events_path = request_path.with_name(f"{request_id}.events.jsonl")
    try:
        request_path.rename(processing_path)
    except (FileNotFoundError, OSError):
        return

    def emit(event: dict[str, Any]) -> None:
        with events_path.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps({"request_id": request_id, **event}, ensure_ascii=False) + "\n")
            handle.flush()

    try:
        request = json.loads(processing_path.read_text(encoding="utf-8"))
        emit({"type": "started", "engine": "SoulX-Singer-SVC", "device": renderer.device})
        result = renderer.render(request)
        emit({"type": "done", **result})
    except Exception as exc:  # noqa: BLE001 - keep the persistent worker alive.
        emit({
            "type": "error",
            "message": str(exc),
            "traceback": traceback.format_exc(limit=12),
        })
    finally:
        processing_path.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
