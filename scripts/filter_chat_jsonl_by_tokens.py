#!/usr/bin/env python3
"""Filter chat JSONL rows by tokenizer chat-template token length."""

from __future__ import annotations

import argparse
import json
from pathlib import Path

from transformers import AutoTokenizer


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--max-tokens", type=int, required=True)
    parser.add_argument("--min-assistant-tokens", type=int, default=16)
    return parser.parse_args()


def main() -> None:
    args = parse_args()
    tokenizer = AutoTokenizer.from_pretrained(args.model)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    kept = 0
    dropped = 0
    max_seen = 0
    with args.input.expanduser().resolve().open(encoding="utf-8") as source, args.output.expanduser().resolve().open("w", encoding="utf-8") as destination:
        for line in source:
            if not line.strip():
                continue
            row = json.loads(line)
            messages = row["messages"]
            tokens = tokenizer.apply_chat_template(messages, return_dict=False)
            assistant_tokens = tokenizer.encode(messages[-1]["content"])
            max_seen = max(max_seen, len(tokens))
            if len(tokens) <= args.max_tokens and len(assistant_tokens) >= args.min_assistant_tokens:
                destination.write(json.dumps(row, ensure_ascii=False, separators=(",", ":")) + "\n")
                kept += 1
            else:
                dropped += 1
    print(json.dumps({"kept": kept, "dropped": dropped, "max_seen": max_seen, "output": str(args.output)}, indent=2))


if __name__ == "__main__":
    main()
