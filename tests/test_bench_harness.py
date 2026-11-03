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
from fastapi import FastAPI, Request  # noqa: E402


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


# ------------------------------------------------- request chain and estimate
def test_chain_estimate_follows_the_panel_vad_rule() -> None:
    # final = max(silence wait, pause speculation start + ASR), then text
    assert run_bench.estimate_speech_end_to_text_ms(300, 1000, 24) == pytest.approx(24 * 32 + 1000)
    assert run_bench.estimate_speech_end_to_text_ms(1500, 1000, 24) == pytest.approx(6 * 32 + 1500 + 1000)
    assert run_bench.estimate_speech_end_to_text_ms(1500, 0, 16) == pytest.approx(6 * 32 + 1500)


def test_chain_suite_reports_request_level_names_and_failures(live_service, tmp_path: Path) -> None:
    app, url = live_service
    (tmp_path / "a.wav").write_bytes(samples_to_wav(np.zeros(16000, dtype=np.float32)))
    (tmp_path / "b.wav").write_bytes(samples_to_wav(np.zeros(16000, dtype=np.float32)))
    manifest = tmp_path / "m.jsonl"
    manifest.write_text("\n".join(json.dumps(r) for r in (
        {"audio": "a.wav", "text": "Open the file."}, {"audio": "b.wav", "text": "Loop forever."})), encoding="utf-8")
    texts = iter(["open the file.", "loop forever."])

    async def transcribe(wav_bytes, *, model, **kw):
        return {"text": next(texts)}

    async def complete(messages, **kw):
        if "loop" in messages[-1]["content"]:
            return {"choices": [{"message": {"content": "loop " * 50}, "finish_reason": "length"}]}
        return {"choices": [{"message": {"content": "Open the file."}, "finish_reason": "stop"}]}

    app.state.registry.audiocpp.transcribe = transcribe
    app.state.registry.llm.complete = complete
    args = _args(manifest=str(manifest), cold=False, warmup=False, translate=False, silence_frames=24)
    with httpx.Client(base_url=url, trust_env=False) as client:
        result = run_bench.suite_chain(args, client)
    assert result["measurement"] == "request"
    assert "speech_end_to_text_ms" not in result                # no unqualified end-to-end name
    assert "Excludes the VAD end-of-speech wait" in result["scope"]
    assert result["estimate"]["kind"] == "model estimate, not measured"
    assert result["text_failures"] == 1
    ok, failed = result["cases"]
    assert ok["asr_plus_text_request_ms"] == pytest.approx(ok["asr_request_ms"] + ok["text_request_ms"], abs=0.2)
    assert ok["estimated_speech_end_to_text_ms"] == pytest.approx(
        run_bench.estimate_speech_end_to_text_ms(ok["asr_request_ms"], ok["text_request_ms"], 24), abs=0.2)
    assert failed["reason"] == "generation_limit" and failed["status"] == 502
    assert "estimated_speech_end_to_text_ms" not in failed     # failures never enter latency figures
    assert result["asr_plus_text_request_ms"]["n"] == 1


def test_text_suite_counts_failures_instead_of_crashing(live_service) -> None:
    app, url = live_service

    async def complete(messages, **kw):
        return {"choices": [{"message": {"content": "x"}, "finish_reason": "length"}]}

    app.state.registry.llm.complete = complete
    with httpx.Client(base_url=url, trust_env=False) as client:
        result = run_bench.suite_text(_args(suite="translation"), client)
    assert result["failures"] == 14 and result["latency_ms"]["n"] == 0


def test_legacy_template_puts_the_task_before_the_context() -> None:
    from voiceservice.processing import build_messages

    legacy = run_bench.legacy_build_messages("x", "correct_ru", context="CTX")[0]["content"]
    current = build_messages("x", "correct_ru", context="CTX")[0]["content"]
    assert legacy.index("Task: ") < legacy.index("CTX")
    assert current.index("CTX") < current.index("Task: ")
    assert sorted(legacy.splitlines()) == sorted(current.splitlines())   # same parts, other order


# --------------------------------------------------------- panel timing log
def test_panel_timing_pairs_each_utterance_and_skips_ambiguous_ones() -> None:
    import panel_timing

    events = [
        {"event": "session_start", "t_ms": 0},
        {"event": "segment_end", "segment": 0, "last_speech_ms": 1000, "audio_s": 2.0, "t_ms": 1768},
        {"event": "final", "segment": 0, "chars": 20, "t_ms": 1900},
        {"event": "text_request", "chars": 20, "t_ms": 1901},
        {"event": "text_result", "chars": 20, "t_ms": 3200},
        {"event": "inserted", "chars": 21, "automatic": True, "clipboard": False, "t_ms": 3215},
        # two finals before one result: batched, not paired
        {"event": "segment_end", "segment": 1, "last_speech_ms": 5000, "audio_s": 1.0, "t_ms": 5768},
        {"event": "final", "segment": 1, "chars": 10, "t_ms": 5800},
        {"event": "segment_end", "segment": 2, "last_speech_ms": 5900, "audio_s": 1.0, "t_ms": 6668},
        {"event": "final", "segment": 2, "chars": 10, "t_ms": 6700},
        {"event": "text_result", "chars": 20, "t_ms": 7000},
        # a new session restarts segment ids
        {"event": "session_start", "t_ms": 9000},
        {"event": "segment_end", "segment": 0, "last_speech_ms": 10000, "audio_s": 2.0, "t_ms": 10768},
        {"event": "final", "segment": 0, "chars": 20, "t_ms": 11000},
        {"event": "text_failed", "chars": 20, "t_ms": 11500},
    ]
    result = panel_timing.analyze(events)
    assert result["sessions"] == 2
    assert result["utterances"] == 2 and result["ambiguous"] == 1 and result["text_failures"] == 1
    first = result["rows"][0]
    assert first["vad_wait_ms"] == 768
    assert first["speech_end_to_final_ms"] == 900
    assert first["final_to_text_ms"] == 1300
    assert first["speech_end_to_insert_ms"] == 2215
    last = result["rows"][1]                     # segment 2 of session 1: its own result, no insert
    assert last["segment"] == 2 and "speech_end_to_insert_ms" not in last
    assert result["not_inserted"] == 1


def test_prefix_suite_shows_more_reuse_with_the_current_order(live_service) -> None:
    """A fake llama-server reports cache_n as the character prefix shared with
    the previous prompt (cache_prompt with one slot, simulated on characters,
    not tokens). Structural check only; real numbers come from the laptop."""
    app, url = live_service
    fake = FastAPI()
    state = {"last": ""}

    @fake.post("/v1/chat/completions")
    async def chat(request: Request):
        body = await request.json()
        prompt = "".join(m["role"] + m["content"] for m in body["messages"])
        shared = 0
        for a, b in zip(prompt, state["last"]):
            if a != b:
                break
            shared += 1
        state["last"] = prompt
        return {"choices": [{"message": {"content": "OK."}, "finish_reason": "stop"}],
                "timings": {"cache_n": shared, "prompt_n": len(prompt) - shared}}

    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    server = uvicorn.Server(uvicorn.Config(fake, host="127.0.0.1", port=port, log_level="error"))
    thread = threading.Thread(target=server.run, daemon=True)
    thread.start()
    while not server.started:
        time.sleep(0.05)

    async def complete(messages, **kw):          # the service's warm-up reaches the same fake
        async with httpx.AsyncClient(trust_env=False) as c:
            r = await c.post(f"http://127.0.0.1:{port}/v1/chat/completions", json={"messages": messages})
            return r.json()

    async def transcribe(wav_bytes, *, model, **kw):
        return {"text": ""}

    app.state.registry.llm.complete = complete
    app.state.registry.audiocpp.transcribe = transcribe
    try:
        with httpx.Client(base_url=url, trust_env=False) as client:
            result = run_bench.suite_prefix(_args(llm_url=f"http://127.0.0.1:{port}", warmup_wait=0.5), client)
    finally:
        server.should_exit = True
        thread.join(timeout=10)
    legacy = result["legacy"]["prompt_n_after_language_switch"]["p50"]
    current = result["current"]["prompt_n_after_language_switch"]["p50"]
    assert current < legacy
    warm = result["first_request_after_reset"]
    assert warm["with_warmup"]["cache_n"] > warm["without_warmup"]["cache_n"]
