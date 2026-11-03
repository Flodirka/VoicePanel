"""The benchmark harness: metrics and a run of its suites against the real
service application with fake engines (no models)."""
from __future__ import annotations

import argparse
import json
import socket
import sys
import threading
import time
from pathlib import Path

import httpx
import numpy as np
import pytest
import uvicorn

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "bench"))

import run_bench  # noqa: E402
from metrics import cer, chrf, edit_ratio, normalize_for_wer, preserved, wer  # noqa: E402
from voiceservice.app import create_app  # noqa: E402
from voiceservice.audio import samples_to_wav  # noqa: E402


def test_word_and_character_error_rates() -> None:
    assert wer("Open the file main.cpp.", "open the file main.cpp") == 0.0
    assert wer("один два три четыре", "один два три") == pytest.approx(0.25)
    assert cer("ёлка", "елка") == 0.0
    assert normalize_for_wer("Run src/app.py, then stop.") == "run src/app.py then stop"


def test_chrf_and_edit_ratio_bounds() -> None:
    assert chrf("Character progression.", "Character progression.") == pytest.approx(100.0)
    assert chrf("Character progression.", "Something else entirely") < 40
    assert edit_ratio("abc", "abc") == 0.0
    assert preserved(["main.cpp", "LoadConfig"], "Open main.cpp") == (1, 2, ["LoadConfig"])


def test_every_case_is_well_formed() -> None:
    for name in ("correction", "translation"):
        rows = run_bench.load_jsonl(ROOT / "tools" / "bench" / "cases" / f"{name}.jsonl")
        assert len({r["id"] for r in rows}) == len(rows)
        for row in rows:
            for token in row.get("protect", []):
                assert token in row["input"], (row["id"], token)
                target = row.get("expected") or row.get("reference")
                assert token in target, (row["id"], token)


@pytest.fixture()
def live_service(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    (tmp_path / "runtime").mkdir()
    cfg = tmp_path / "config.yaml"
    cfg.write_text("backends:\n  audiocpp_clone:\n    enabled: false\n", encoding="utf-8")
    app = create_app(cfg)
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    server = uvicorn.Server(uvicorn.Config(app, host="127.0.0.1", port=port, log_level="error"))
    thread = threading.Thread(target=server.run, daemon=True)
    thread.start()
    deadline = time.monotonic() + 10
    while not server.started and time.monotonic() < deadline:
        time.sleep(0.05)
    yield app, f"http://127.0.0.1:{port}"
    server.should_exit = True
    thread.join(timeout=10)


def _args(**kw) -> argparse.Namespace:
    base = dict(repeat=1, discard_first=False, profile="general", temperature=None, context=None,
                llm_url=None, template="voicepanel", user_template=None, sampling=None, suite="correction",
                model=None, prompt=None, prefix_step=0.0)
    base.update(kw)
    return argparse.Namespace(**base)


def test_text_suite_scores_an_echo_model(live_service) -> None:
    app, url = live_service

    async def echo(messages, **kw):
        return {"choices": [{"message": {"content": messages[-1]["content"]}, "finish_reason": "stop"}]}

    app.state.registry.llm.complete = echo
    with httpx.Client(base_url=url, trust_env=False) as client:
        result = run_bench.suite_text(_args(), client)
    assert result["unchanged_kept_rate"] == 1.0          # an echo never edits correct text
    assert result["protected_token_rate"] == 1.0
    assert result["edit_vs_expected_mean"] > 0          # but it fixes nothing either
    assert result["latency_ms"]["n"] == 25


def test_asr_suite_reads_a_manifest(live_service, tmp_path: Path) -> None:
    app, url = live_service
    wav = samples_to_wav(np.zeros(16000, dtype=np.float32))
    (tmp_path / "a.wav").write_bytes(wav)
    manifest = tmp_path / "asr.jsonl"
    manifest.write_text(json.dumps({"audio": "a.wav", "text": "Hello there.", "tags": ["en"]}) + "\n", encoding="utf-8")

    async def transcribe(wav_bytes, *, model, **kw):
        return {"text": "hello there"}

    app.state.registry.audiocpp.transcribe = transcribe
    with httpx.Client(base_url=url, trust_env=False) as client:
        result = run_bench.suite_asr(_args(manifest=str(manifest)), client)
    assert result["wer_mean"] == 0.0
    assert result["by_tag"]["en"]["n"] == 1
    assert result["rtf"]["n"] == 1
