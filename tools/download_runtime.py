"""Fetch locked runtime artifacts over HTTPS; verify before extraction, never execute."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import urllib.parse
import urllib.request
import zipfile


class HttpsRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        if urllib.parse.urlsplit(newurl).scheme != "https":
            raise ValueError("HTTPS is required for download redirects")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def open_url(url: str):
    return urllib.request.build_opener(HttpsRedirects()).open(url, timeout=60)


def verify_file(path: Path, expected: str) -> None:
    if not re.fullmatch(r"[a-f0-9]{64}", expected):
        raise ValueError("Invalid SHA-256 in download lock")
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    if digest.hexdigest() != expected:
        raise ValueError(f"SHA-256 mismatch: {path.name}; file was not installed")


def download_file(url: str, destination: Path, expected: str) -> None:
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https":
        raise ValueError("HTTPS is required for downloads")
    if parsed.hostname not in {"github.com", "raw.githubusercontent.com", "huggingface.co"}:
        raise ValueError("Download source is not an approved upstream host")
    if not re.fullmatch(r"[a-f0-9]{64}", expected):
        raise ValueError("Invalid SHA-256 in download lock")
    if destination.exists():
        verify_file(destination, expected)
        return
    destination.parent.mkdir(parents=True, exist_ok=True)
    partial = destination.with_name(destination.name + ".part")
    try:
        with open_url(url) as response, partial.open("wb") as output:
            shutil.copyfileobj(response, output, length=1024 * 1024)
        verify_file(partial, expected)
        partial.replace(destination)
    finally:
        partial.unlink(missing_ok=True)


def inside(root: Path, relative: str) -> Path:
    result = (root / relative).resolve()
    if not result.is_relative_to(root.resolve()):
        raise ValueError("Destination is outside the runtime checkout")
    return result


def extract_archive(archive: Path, destination: Path, strip_prefix: str = "") -> None:
    with zipfile.ZipFile(archive) as source:
        members = []
        if sum(item.file_size for item in source.infolist()) > 8 * 1024**3:
            raise ValueError("Expanded archive exceeds the size limit")
        for item in source.infolist():
            name = item.filename.replace("\\", "/")
            path = PurePosixPath(name)
            if path.is_absolute() or ".." in path.parts or ":" in name:
                raise ValueError("Unsafe archive path")
            if stat.S_ISLNK(item.external_attr >> 16):
                raise ValueError("Archive symlink is not allowed")
            if strip_prefix:
                if not name.startswith(strip_prefix):
                    raise ValueError("Unexpected archive prefix")
                name = name[len(strip_prefix):]
            if not name or item.is_dir():
                continue
            members.append((item, inside(destination, name)))
        # Validate every entry before writing any archive content.
        for item, target in members:
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.open(item) as data, target.open("wb") as output:
                shutil.copyfileobj(data, output, length=1024 * 1024)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--group", choices=("runtime", "extras", "clone"), default="runtime")
    parser.add_argument("--verify-only", action="store_true", help="check cached downloads without extracting or fetching")
    args = parser.parse_args()
    root = Path(__file__).resolve().parent.parent
    lock = json.loads((root / "tools/downloads.lock.json").read_text(encoding="utf-8"))
    for entry in lock["downloads"]:
        if entry["group"] != args.group:
            continue
        destination = inside(root, entry["destination"])
        print(("Checking: " if args.verify_only else "Preparing: ") + entry["name"], flush=True)
        if args.verify_only:
            verify_file(destination, entry["sha256"])
            continue
        download_file(entry["url"], destination, entry["sha256"])
        if entry.get("extract_to"):
            extract_archive(destination, inside(root, entry["extract_to"]), entry.get("strip_prefix", ""))
    print("Verified. No downloaded program was executed.")


if __name__ == "__main__":
    main()
