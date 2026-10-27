"""Launcher for the Voice AI Service from a source checkout.

Adds ``<repo>/service`` to ``sys.path`` explicitly so the service starts with any
Python build, including interpreters that do not put the working directory on
``sys.path`` (or ignore ``PYTHONPATH``).

Usage:
    python tools/run_service.py [--config config.yaml] [--host 127.0.0.1] [--port 8080]
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "service"))

from voiceservice.__main__ import main  # noqa: E402

if __name__ == "__main__":
    sys.argv[0] = "voiceservice"
    main()
