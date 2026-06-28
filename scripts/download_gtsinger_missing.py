#!/usr/bin/env python3
"""Resume GTSinger folders omitted by Hugging Face's large-repo file listing."""

from __future__ import annotations

import argparse
from concurrent.futures import FIRST_COMPLETED, Future, ThreadPoolExecutor, wait
from pathlib import Path

from huggingface_hub import HfApi, hf_hub_download
from huggingface_hub.hf_api import RepoFile


REPO_ID = "AaronZ345/GTSinger"
DEFAULT_ROOTS = ("Japanese", "Korean", "Russian", "Spanish", "processed")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--local-dir", type=Path, required=True)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--root", action="append", dest="roots")
    return parser.parse_args()


def download_file(local_dir: Path, entry: RepoFile) -> int:
    destination = local_dir / entry.path
    if destination.is_file() and destination.stat().st_size == entry.size:
        return 0

    hf_hub_download(
        repo_id=REPO_ID,
        filename=entry.path,
        repo_type="dataset",
        local_dir=local_dir,
    )
    return entry.size


def drain_one(pending: set[Future[int]]) -> int:
    completed, _ = wait(pending, return_when=FIRST_COMPLETED)
    downloaded = 0
    for future in completed:
        pending.remove(future)
        downloaded += future.result()
    return downloaded


def main() -> None:
    args = parse_args()
    roots = tuple(args.roots or DEFAULT_ROOTS)
    args.local_dir.mkdir(parents=True, exist_ok=True)

    api = HfApi()
    discovered_files = 0
    discovered_bytes = 0
    downloaded_bytes = 0
    pending: set[Future[int]] = set()
    max_pending = max(args.workers * 4, 1)

    with ThreadPoolExecutor(max_workers=args.workers) as executor:
        for root in roots:
            print(f"Scanning {root}...", flush=True)
            for entry in api.list_repo_tree(
                REPO_ID,
                path_in_repo=root,
                repo_type="dataset",
                recursive=True,
                expand=False,
            ):
                if not isinstance(entry, RepoFile):
                    continue
                discovered_files += 1
                discovered_bytes += entry.size
                pending.add(executor.submit(download_file, args.local_dir, entry))
                if len(pending) >= max_pending:
                    downloaded_bytes += drain_one(pending)
                if discovered_files % 1000 == 0:
                    print(
                        f"Discovered {discovered_files} files "
                        f"({discovered_bytes / 1e9:.2f} GB), "
                        f"downloaded {downloaded_bytes / 1e9:.2f} GB",
                        flush=True,
                    )

        while pending:
            downloaded_bytes += drain_one(pending)

    print(
        f"Complete: {discovered_files} files, "
        f"{discovered_bytes / 1e9:.2f} GB verified, "
        f"{downloaded_bytes / 1e9:.2f} GB downloaded",
        flush=True,
    )


if __name__ == "__main__":
    main()
