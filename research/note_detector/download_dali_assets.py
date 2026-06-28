#!/usr/bin/env python3
"""Download public DALI metadata and, when authorized, Zenodo archives."""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any


PUBLIC_ASSETS = {
    "dali_index_1.0.json": "https://zenodo.org/records/13930497/files/dali_index_1.0.json?download=1",
    "dali_v1_metadata.json": "https://raw.githubusercontent.com/gabolsgabs/DALI/master/code/DALI/files/dali_v1_metadata.json",
}


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Prepare DALI raw data folders and download assets that are publicly accessible."
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("data/raw/dali"),
        help="Destination root for DALI assets.",
    )
    parser.add_argument(
        "--record-id",
        default="2577915",
        help="Zenodo DALI record id. v1 is 2577915; latest v2 is 3576083.",
    )
    parser.add_argument(
        "--zenodo-token",
        default=os.environ.get("ZENODO_TOKEN"),
        help="Optional Zenodo token with granted access. Defaults to ZENODO_TOKEN env var.",
    )
    parser.add_argument(
        "--skip-restricted",
        action="store_true",
        help="Only download public metadata/index and do not probe restricted Zenodo files.",
    )
    return parser.parse_args()


def request_json(url: str, token: str | None = None) -> dict[str, Any]:
    headers = {"Accept": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request) as response:
        return json.loads(response.read().decode("utf-8"))


def download(url: str, destination: Path, token: str | None = None) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    headers = {}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers)
    with urllib.request.urlopen(request) as response, destination.open("wb") as handle:
        handle.write(response.read())


def download_public_assets(output_dir: Path) -> None:
    metadata_dir = output_dir / "metadata"
    for filename, url in PUBLIC_ASSETS.items():
        destination = metadata_dir / filename
        print(f"Downloading public asset: {filename}")
        download(url, destination)


def extract_files(record: dict[str, Any]) -> list[dict[str, Any]]:
    files = record.get("files")
    if isinstance(files, list):
        return files
    return []


def file_download_url(file_info: dict[str, Any]) -> str | None:
    links = file_info.get("links", {})
    if isinstance(links, dict):
        for key in ("self", "download"):
            value = links.get(key)
            if isinstance(value, str):
                return value
    return None


def file_name(file_info: dict[str, Any]) -> str:
    for key in ("key", "filename"):
        value = file_info.get(key)
        if isinstance(value, str) and value:
            return value
    return "dali_zenodo_file"


def download_restricted_record(output_dir: Path, record_id: str, token: str | None) -> bool:
    record_url = f"https://zenodo.org/api/records/{record_id}"
    print(f"Checking Zenodo record: {record_url}")
    try:
        record = request_json(record_url, token)
    except urllib.error.HTTPError as exc:
        print(f"Zenodo request failed: HTTP {exc.code}", file=sys.stderr)
        return False

    files = extract_files(record)
    if not files:
        print(
            "Zenodo returned no files. DALI is restricted; request access in the browser, "
            "then rerun with ZENODO_TOKEN after access is granted."
        )
        print(f"Request access: {record.get('links', {}).get('self_html', 'https://zenodo.org/records/' + record_id)}")
        return False

    archive_dir = output_dir / "zenodo"
    for file_info in files:
        url = file_download_url(file_info)
        if url is None:
            print(f"Skipping file without download URL: {file_info}")
            continue
        destination = archive_dir / file_name(file_info)
        print(f"Downloading restricted file: {destination.name}")
        download(url, destination, token)
    return True


def main() -> int:
    args = parse_args()
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)

    download_public_assets(output_dir)

    if not args.skip_restricted:
        download_restricted_record(output_dir, args.record_id, args.zenodo_token)

    print(f"Done: {output_dir}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
