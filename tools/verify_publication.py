"""Check tracked public files without printing matched secrets or private content."""
from __future__ import annotations

import html
import json
from pathlib import Path
import re
import subprocess
import sys
import urllib.parse
import xml.etree.ElementTree as ET


PRIVATE_DIRS = {"runtime", "downloads", "attachments", "research-notes", ".workbuddy-ai", ".freebuff", ".zwork", ".codex", ".agents", "test-records"}
PRIVATE_EXTENSIONS = {".wav", ".mp3", ".ogg", ".flac", ".m4a", ".mp4", ".gguf", ".bin", ".exe", ".dll", ".pem", ".key", ".p12", ".pfx", ".sqlite", ".db"}
SECRET_RULES = {
    "credential-format": re.compile(r"(?:gh[pousr]_[A-Za-z0-9]{30,}|github_pat_[A-Za-z0-9_]{30,}|sk-(?:proj-)?[A-Za-z0-9_-]{24,}|AKIA[A-Z0-9]{16}|[0-9]{8,12}:[A-Za-z0-9_-]{35})"),
    "private-key": re.compile(r"-----BEGIN (?:RSA |EC |OPENSSH )?PRIVATE KEY-----"),
    "literal-credential": re.compile(r'''(?i)(?:api[_-]?key|auth_token|client_secret|password|access_token|secret_key)["']?\s*[:=]\s*["']?(?!synthetic-)[A-Za-z0-9_/+=.-]{16,}'''),
    "url-credential": re.compile(r"https?://[^\s/@:]+:[^\s/@]+@"),
    "personal-home-path": re.compile(r"(?i)[a-z]:[\\/]+Users[\\/]+(?!name(?:[\\/]|\b)|Public(?:[\\/]|\b))[^\\/\s\"']+"),
}


def scan_files(root: Path, paths: list[str]) -> list[dict[str, str]]:
    findings = []
    for name in paths:
        path = Path(name)
        source = root / path
        if any(part in PRIVATE_DIRS or part.startswith("test-state-") for part in path.parts) or path.suffix.lower() in PRIVATE_EXTENSIONS or path.name in {"config.yaml", "config.local.yaml"} or path.name.startswith(".env") or any(part == "build" or part.startswith(("build-", "build_")) for part in path.parts[:-1]):
            findings.append({"path": name, "rule": "private-artifact"})
        if source.is_symlink():
            findings.append({"path": name, "rule": "symlink"})
            continue
        if not source.is_file():
            findings.append({"path": name, "rule": "missing-file"})
            continue
        if source.suffix.lower() in {".png", ".ttf"}:
            continue
        try:
            text = source.read_text(encoding="utf-8-sig")
        except UnicodeDecodeError:
            findings.append({"path": name, "rule": "unexpected-binary"})
            continue
        for rule, pattern in SECRET_RULES.items():
            if pattern.search(text):
                findings.append({"path": name, "rule": rule})
        if source.suffix == ".svg":
            try:
                svg = ET.fromstring(text)
                for element in svg.iter():
                    if element.tag.split("}")[-1] in {"script", "foreignObject"} or any(k.lower().startswith("on") or (k.endswith("href") and re.match(r"(?:https?:|javascript:)", v)) for k, v in element.attrib.items()):
                        findings.append({"path": name, "rule": "active-svg"})
            except ET.ParseError:
                findings.append({"path": name, "rule": "invalid-svg"})
        if source.suffix == ".md":
            links = re.findall(r'''(?:href|src)=["']([^"']+)["']|\]\(([^)]+)\)''', text)
            for pair in links:
                target = html.unescape(pair[0] or pair[1]).strip("<>").split("#")[0]
                if not target or urllib.parse.urlsplit(target).scheme:
                    continue
                if not (source.parent / urllib.parse.unquote(target)).exists():
                    findings.append({"path": name, "rule": "missing-local-link"})
        if ".github/workflows" in path.as_posix():
            if "pull_request_target" in text or re.search(r":\s*write\b", text):
                findings.append({"path": name, "rule": "privileged-workflow"})
            for action in re.findall(r"uses:\s*([^\s]+)", text):
                if not re.fullmatch(r"[^@]+@[a-f0-9]{40}", action):
                    findings.append({"path": name, "rule": "unpinned-action"})
    return findings


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    paths = subprocess.check_output(["git", "ls-files", "-z"], cwd=root).decode().split("\0")
    paths = [p for p in paths if p]
    findings = scan_files(root, paths)
    lock = json.loads((root / "tools/downloads.lock.json").read_text(encoding="utf-8"))
    for item in lock["downloads"]:
        url = urllib.parse.urlsplit(item["url"])
        if url.scheme != "https" or not re.fullmatch(r"[a-f0-9]{64}", item["sha256"]) or "/resolve/main/" in item["url"] or "/resolve/master/" in item["url"] or "/releases/latest/" in item["url"]:
            findings.append({"path": "tools/downloads.lock.json", "rule": "unlocked-download"})
    print(json.dumps({"files_checked": len(paths), "findings": findings}, indent=2))
    return bool(findings)


if __name__ == "__main__":
    sys.exit(main())
