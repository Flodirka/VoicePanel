"""Warm-up next to real requests: ordering, deduplication, failures, shutdown.

The service runs in a real uvicorn server thread so that HTTP requests overlap
the way they do when the panel starts a session and speaks at once. Engines
are fakes gated by threading events.
"""
from __future__ import annotations

import asyncio
import socket
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import httpx
import numpy as np
import pytest
import uvicorn
from fastapi.testclient import TestClient

from voiceservice.app import create_app
from voiceservice.audio import samples_to_wav
from voiceservice.backends.process import ManagedProcess, ProcessError

SPEECH = samples_to_wav(np.full(16000, 0.2, dtype=np.float32))


def _config(tmp_path: Path, extra: str = "") -> Path:
    (tmp_path / "runtime").mkdir(exist_ok=True)
    cfg = tmp_path / "config.yaml"
    cfg.write_text("backends:\n  audiocpp_clone:\n    enabled: false\n" + extra, encoding="utf-8")
    return cfg


@pytest.fixture()
def live(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    app = create_app(_config(tmp_path))
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    server = uvicorn.Server(uvicorn.Config(app, host="127.0.0.1", port=port, log_level="error"))
    thread = threading.Thread(target=server.run, daemon=True)
    thread.start()
    deadline = time.monotonic() + 10
    while not server.started and time.monotonic() < deadline:
        time.sleep(0.05)
    client = httpx.Client(base_url=f"http://127.0.0.1:{port}", timeout=20, trust_env=False)
    yield app, client
    client.close()
    server.should_exit = True
    thread.join(timeout=10)


def _wait(predicate, timeout: float = 5.0) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(0.01)
    return False


def _is_silence(wav: bytes) -> bool:
    return not any(wav[44:])


def test_first_asr_request_waits_for_warmup_and_keeps_its_own_transcript(live) -> None:
    app, client = live
    registry = app.state.registry
    calls: list[str] = []
    gate = threading.Event()

    async def transcribe(wav, *, model, **kw):
        kind = "warm" if _is_silence(wav) else "real"
        calls.append(kind)
        if kind == "warm":
            await asyncio.to_thread(gate.wait, 5)      # the model is "loading"
            return {"text": "嗯。"}                    # what a model may say about silence
        return {"text": "Open the file main.cpp."}

    registry.audiocpp.transcribe = transcribe
    assert client.post("/v1/warmup", json={"mode": "dictation"}).json()["scheduled"] == ["asr"]
    assert _wait(lambda: calls == ["warm"])
    with ThreadPoolExecutor(1) as pool:
        real = pool.submit(client.post, "/v1/audio/transcriptions",
                           files={"file": ("a.wav", SPEECH, "audio/wav")}, data={"response_format": "json"})
        time.sleep(0.2)
        assert not real.done() and calls == ["warm"]    # queued behind the load, not run beside it
        gate.set()
        resp = real.result(timeout=10)
    assert resp.status_code == 200
    assert resp.json()["text"] == "Open the file main.cpp."   # never the warm-up's output
    assert calls == ["warm", "real"]
    assert _wait(lambda: not registry.warming)


def test_warmup_skips_the_load_when_a_real_request_loaded_the_model_first(live, monkeypatch) -> None:
    app, client = live
    registry = app.state.registry
    calls: list[str] = []
    state = {"loaded": False}
    gate = threading.Event()

    async def transcribe(wav, *, model, **kw):
        kind = "warm" if _is_silence(wav) else "real"
        calls.append(kind)
        if kind == "real":
            await asyncio.to_thread(gate.wait, 5)
            state["loaded"] = True
        return {"text": "real"}

    async def loaded_models():
        return [{"id": "qwen3-asr", "loaded": state["loaded"]}]

    monkeypatch.setattr(type(registry.audiocpp.proc), "running", property(lambda self: True))
    registry.audiocpp.transcribe = transcribe
    registry.audiocpp.loaded_models = loaded_models
    with ThreadPoolExecutor(1) as pool:
        real = pool.submit(client.post, "/v1/audio/transcriptions",
                           files={"file": ("a.wav", SPEECH, "audio/wav")})
        assert _wait(lambda: calls == ["real"])
        client.post("/v1/warmup", json={"mode": "dictation"})   # waits for the lock
        time.sleep(0.1)
        gate.set()
        assert real.result(timeout=10).status_code == 200
    assert _wait(lambda: not registry.warming)
    assert calls == ["real"]                                    # no silent request after the load


def test_warmup_and_first_text_request_both_complete_with_their_own_results(live) -> None:
    app, client = live
    registry = app.state.registry
    seen: list[dict] = []
    gate = threading.Event()

    async def complete(messages, **kw):
        seen.append({"max_tokens": kw.get("max_tokens"), "user": messages[-1]["content"]})
        if kw.get("max_tokens") == 1:
            await asyncio.to_thread(gate.wait, 5)
            return {"choices": [{"message": {"content": "O"}, "finish_reason": "length"}]}
        return {"choices": [{"message": {"content": "Нам нужно исправить эту ошибку."}, "finish_reason": "stop"}]}

    async def transcribe(wav, *, model, **kw):
        return {"text": ""}

    registry.llm.complete = complete
    registry.audiocpp.transcribe = transcribe
    client.post("/v1/warmup", json={"mode": "auto_correct", "context": "Glossary."})
    assert _wait(lambda: len(seen) == 1)
    resp = client.post("/v1/text/process", json={"text": "нам нужно исправить эта ошибка", "mode": "correct_ru",
                                                 "context": "Glossary."})
    gate.set()
    assert resp.status_code == 200
    assert resp.json()["result"] == "Нам нужно исправить эту ошибку."
    assert seen[0] == {"max_tokens": 1, "user": "OK."}
    assert seen[1]["user"] == "нам нужно исправить эта ошибка"
    assert _wait(lambda: not registry.warming)


def test_duplicate_warmups_are_scheduled_once(live) -> None:
    app, client = live
    registry = app.state.registry
    calls: list[str] = []
    gate = threading.Event()

    async def transcribe(wav, *, model, **kw):
        calls.append("warm")
        await asyncio.to_thread(gate.wait, 5)
        return {"text": ""}

    registry.audiocpp.transcribe = transcribe
    assert client.post("/v1/warmup", json={}).json()["scheduled"] == ["asr"]
    assert _wait(lambda: calls == ["warm"])
    for _ in range(3):
        assert client.post("/v1/warmup", json={}).json()["scheduled"] == []
    gate.set()
    assert _wait(lambda: not registry.warming)
    assert calls == ["warm"]


@pytest.mark.parametrize("error", [httpx.ConnectError("engine is down"), ProcessError("binary not found")])
def test_failed_warmup_is_contained_and_can_run_again(live, error) -> None:
    app, client = live
    registry = app.state.registry
    attempts: list[int] = []

    async def transcribe(wav, *, model, **kw):
        attempts.append(1)
        raise error

    registry.audiocpp.transcribe = transcribe
    assert client.post("/v1/warmup", json={}).status_code == 202
    assert _wait(lambda: attempts and not registry.warming)
    assert client.post("/v1/warmup", json={}).json()["scheduled"] == ["asr"]
    assert _wait(lambda: len(attempts) == 2 and not registry.warming)
    assert client.get("/health").status_code == 200


def test_whisper_default_is_not_loaded_through_audiocpp(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    with TestClient(create_app(_config(tmp_path, "asr:\n  default: whisper\n")), base_url="http://127.0.0.1") as client:
        registry = client.app.state.registry

        async def transcribe(wav, *, model, **kw):
            raise AssertionError("audio.cpp must not be used for a whisper default")

        registry.audiocpp.transcribe = transcribe
        assert client.post("/v1/warmup", json={"mode": "dictation"}).status_code == 202
        assert _wait(lambda: not registry.warming)


def test_shutdown_cancels_a_pending_warmup(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    outcome: list[str] = []
    with TestClient(create_app(_config(tmp_path)), base_url="http://127.0.0.1") as client:
        registry = client.app.state.registry

        async def transcribe(wav, *, model, **kw):
            try:
                await asyncio.sleep(30)
            except asyncio.CancelledError:
                outcome.append("cancelled")
                raise
            outcome.append("finished")
            return {"text": ""}

        registry.audiocpp.transcribe = transcribe
        client.post("/v1/warmup", json={"mode": "dictation"})
        assert _wait(lambda: "asr" in registry.warming)
    assert outcome == ["cancelled"]
    assert not registry.background_tasks


def test_transport_error_makes_the_next_request_probe_again(tmp_path: Path) -> None:
    async def scenario() -> None:
        proc = ManagedProcess(name="fake", command=[sys.executable, "-c", "import time; time.sleep(60)"],
                              host="127.0.0.1", port=1, log_dir=tmp_path / "logs")
        probes: list[float] = []

        async def probe(timeout: float = 0.0) -> bool:
            probes.append(timeout)
            return bool(proc._proc is not None and proc._proc.poll() is None)

        proc._probe = probe  # type: ignore[method-assign]
        try:
            await proc.start()
            before = len(probes)
            async with proc.lease():
                pass
            assert len(probes) == before          # healthy: no probe
            with pytest.raises(httpx.ReadTimeout):
                async with proc.lease():
                    raise httpx.ReadTimeout("hung engine")
            async with proc.lease():
                pass
            assert len(probes) > before           # probed again after the failure
            assert proc._leases == 0
        finally:
            await proc.stop()

    asyncio.run(scenario())
