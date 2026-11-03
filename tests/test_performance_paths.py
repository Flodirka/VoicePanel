"""Regression tests for the latency and resource fixes of the performance audit.

All tests are model-free: fake child processes, a fake local HTTP backend and
controlled backend responses.
"""
from __future__ import annotations

import asyncio
import socket
import sys
import threading
import time
from pathlib import Path

import pytest
import uvicorn
from fastapi import FastAPI, Request
from fastapi.testclient import TestClient

from voiceservice.api.text import default_max_tokens
from voiceservice.app import create_app
from voiceservice.backends.audiocpp import AudioCppBackend
from voiceservice.backends.llama import LlamaBackend
from voiceservice.backends.process import ManagedProcess
from voiceservice.config import load_config
from voiceservice.processing import build_messages
from voiceservice.vad import LiveTranscription, speculation_interval


# ----------------------------------------------------------------- processes
def _sleeper(tmp_path: Path) -> ManagedProcess:
    return ManagedProcess(
        name="fake",
        command=[sys.executable, "-c", "import time; time.sleep(60)"],
        host="127.0.0.1",
        port=1,
        log_dir=tmp_path / "logs",
        idle_stop_s=100,
    )


def test_healthy_owned_process_skips_the_per_request_probe(tmp_path: Path) -> None:
    async def scenario() -> None:
        proc = _sleeper(tmp_path)
        probes = []

        async def probe(timeout: float = 0.0) -> bool:
            probes.append(timeout)
            return bool(proc._proc is not None and proc._proc.poll() is None)

        proc._probe = probe  # type: ignore[method-assign]
        try:
            await proc.start()                  # spawn: one probe while waiting for health
            assert len(probes) == 2             # "anything on the port?" + startup wait
            for _ in range(5):
                async with proc.lease():
                    pass
            assert len(probes) == 2             # no probe per request
            proc._proc.kill()                   # the child crashes
            proc._proc.wait()
            await proc.start()                  # detected without a probe, then respawned
            assert proc._proc.poll() is None
            assert len(probes) > 2
        finally:
            await proc.stop()

    asyncio.run(scenario())


def test_spawn_resets_idle_clock(tmp_path: Path) -> None:
    async def scenario() -> None:
        proc = _sleeper(tmp_path)

        async def probe(timeout: float = 0.0) -> bool:
            return bool(proc._proc is not None and proc._proc.poll() is None)

        proc._probe = probe  # type: ignore[method-assign]
        proc._last_used = time.monotonic() - 10_000   # service idle for hours
        try:
            await proc.start()                        # e.g. POST /admin/backends/x/load
            assert time.monotonic() - proc._last_used < 5
        finally:
            await proc.stop()

    asyncio.run(scenario())


# --------------------------------------------------------- pooled HTTP client
def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def test_backend_requests_reuse_one_connection(tmp_path: Path) -> None:
    peers: list[int] = []
    fake = FastAPI()

    @fake.post("/v1/audio/transcriptions")
    async def transcribe(request: Request):
        peers.append(request.client.port)
        return {"text": "ok"}

    port = _free_port()
    server = uvicorn.Server(uvicorn.Config(fake, host="127.0.0.1", port=port, log_level="error"))
    thread = threading.Thread(target=server.run, daemon=True)
    thread.start()
    deadline = time.monotonic() + 10
    while not server.started and time.monotonic() < deadline:
        time.sleep(0.05)

    cfg = load_config(None)["backends"]["audiocpp"]
    cfg = {**cfg, "port": port}
    backend = AudioCppBackend(cfg, tmp_path, tmp_path / "logs")
    backend.proc._owned_and_healthy = lambda: True  # type: ignore[method-assign]

    async def scenario() -> None:
        for _ in range(3):
            assert (await backend.transcribe(b"RIFF", model="qwen3-asr"))["text"] == "ok"
        await backend.proc._close_http()

    try:
        asyncio.run(scenario())
    finally:
        server.should_exit = True
        thread.join(timeout=10)
    assert len(peers) == 3
    assert len(set(peers)) == 1, "each request opened a new connection"


# ------------------------------------------------------------ llama arguments
def test_llama_prompt_cache_is_bounded_by_default() -> None:
    cfg = load_config(None)["backends"]["llama"]
    args = LlamaBackend(cfg).proc.command
    assert args[args.index("--cache-ram") + 1] == "512"
    assert "-ctk" not in args and "-ctv" not in args


def test_llama_memory_options_follow_configuration() -> None:
    cfg = {**load_config(None)["backends"]["llama"], "cache_ram_mib": None,
           "cache_type_k": "q8_0", "cache_type_v": "q8_0"}
    args = LlamaBackend(cfg).proc.command
    assert "--cache-ram" not in args
    assert args[args.index("-ctk") + 1] == "q8_0"
    assert args[args.index("-ctv") + 1] == "q8_0"


# -------------------------------------------------------- prompt cache reuse
def _common_prefix(a: str, b: str) -> int:
    n = 0
    for x, y in zip(a, b):
        if x != y:
            break
        n += 1
    return n


def test_language_switch_keeps_the_context_document_in_the_shared_prefix() -> None:
    context = "Terms: UCLASS, GDScript, ScriptableObject. " * 20
    ru = build_messages("Привет.", "correct_ru", context=context)[0]["content"]
    en = build_messages("Hello.", "correct_en", context=context)[0]["content"]
    tr = build_messages("Привет.", "translate_ru_en", context=context)[0]["content"]
    shared = _common_prefix(ru, en)
    assert ru.index(context) + len(context) <= shared
    assert ru.index(context) + len(context) <= _common_prefix(ru, tr)
    assert ru.rstrip().endswith(ru.split("Task: ", 1)[1].strip())   # the task line comes last


def test_spelling_hints_come_after_the_stable_prefix() -> None:
    context = "Project terminology document."
    plain = build_messages("teh text.", "correct_en", context=context)[0]["content"]
    hinted = build_messages("teh text.", "correct_en", context=context,
                            spelling=[{"word": "teh", "suggestions": ["the"]}])[0]["content"]
    assert hinted.startswith(plain)


# ----------------------------------------------------------- text endpoint
@pytest.fixture()
def service(tmp_path: Path, monkeypatch) -> TestClient:
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    (tmp_path / "runtime").mkdir()
    cfg = tmp_path / "config.yaml"
    cfg.write_text("backends:\n  audiocpp_clone:\n    enabled: false\n", encoding="utf-8")
    with TestClient(create_app(cfg), base_url="http://127.0.0.1") as client:
        yield client


def test_text_processing_caps_generation_when_the_client_sets_no_limit(service, monkeypatch) -> None:
    seen = {}

    async def complete(messages, **kw):
        seen.update(kw)
        return {"choices": [{"message": {"content": "Fixed."}, "finish_reason": "stop"}]}

    monkeypatch.setattr(service.app.state.registry.llm, "complete", complete)
    text = "this are a test."
    resp = service.post("/v1/text/process", json={"text": text, "mode": "correct_en"})
    assert resp.status_code == 200 and resp.json()["result"] == "Fixed."
    assert seen["max_tokens"] == default_max_tokens(text)
    resp = service.post("/v1/text/process", json={"text": text, "mode": "correct_en", "max_tokens": 7})
    assert seen["max_tokens"] == 7


def test_explicit_limit_keeps_truncated_output(service, monkeypatch) -> None:
    async def complete(messages, **kw):
        return {"choices": [{"message": {"content": "Cut"}, "finish_reason": "length"}]}

    monkeypatch.setattr(service.app.state.registry.llm, "complete", complete)
    resp = service.post("/v1/text/process",
                        json={"text": "Keep my words.", "mode": "correct_en", "max_tokens": 1})
    assert resp.status_code == 200 and resp.json()["result"] == "Cut"


# ----------------------------------------------------------------- warm-up
def _wait_idle(registry, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while (registry.warming or registry.background_tasks) and time.monotonic() < deadline:
        time.sleep(0.02)


def test_warmup_loads_asr_and_prefills_the_llm_prefix(service, monkeypatch) -> None:
    registry = service.app.state.registry
    calls: dict[str, list] = {"asr": [], "llm": []}

    async def transcribe(wav, *, model, **kw):
        calls["asr"].append(model)
        return {"text": ""}

    async def complete(messages, **kw):
        calls["llm"].append((messages, kw))
        return {"choices": [{"message": {"content": "O"}}]}

    monkeypatch.setattr(registry.audiocpp, "transcribe", transcribe)
    monkeypatch.setattr(registry.llm, "complete", complete)
    resp = service.post("/v1/warmup", json={"mode": "auto_correct", "context": "Glossary: UCLASS."})
    assert resp.status_code == 202
    assert set(resp.json()["scheduled"]) == {"asr", "llm"}
    _wait_idle(registry)
    assert calls["asr"] == ["qwen3-asr"]
    messages, kw = calls["llm"][0]
    assert kw["max_tokens"] == 1
    real = build_messages("A sentence.", "correct_ru", context="Glossary: UCLASS.")[0]["content"]
    assert _common_prefix(messages[0]["content"], real) > real.index("Glossary: UCLASS.")


def test_warmup_skips_dictation_mode_and_external_servers(service, monkeypatch) -> None:
    registry = service.app.state.registry

    async def transcribe(wav, *, model, **kw):
        return {"text": ""}

    monkeypatch.setattr(registry.audiocpp, "transcribe", transcribe)
    assert service.post("/v1/warmup", json={"mode": "dictation"}).json()["scheduled"] == ["asr"]
    _wait_idle(registry)
    monkeypatch.setattr(registry.llm, "external", True)
    assert "llm" not in service.post("/v1/warmup", json={"mode": "correct_en"}).json()["scheduled"]
    _wait_idle(registry)


def test_warmup_does_not_reload_a_resident_asr_model(service, monkeypatch) -> None:
    registry = service.app.state.registry
    called = []

    async def transcribe(wav, *, model, **kw):
        called.append(model)
        return {"text": ""}

    async def loaded_models():
        return [{"id": "qwen3-asr", "loaded": True}]

    monkeypatch.setattr(type(registry.audiocpp.proc), "running", property(lambda self: True))
    monkeypatch.setattr(registry.audiocpp, "loaded_models", loaded_models)
    monkeypatch.setattr(registry.audiocpp, "transcribe", transcribe)
    service.post("/v1/warmup", json={"llm": False})
    _wait_idle(registry)
    assert called == []


# ------------------------------------------------- server-side live dictation
def test_speculation_interval_grows_with_the_segment() -> None:
    assert speculation_interval(0, 63) == 63
    assert speculation_interval(126, 63) == 63
    assert speculation_interval(400, 63) == 200


def test_speculative_preview_runs_and_awaits_async_callbacks() -> None:
    import numpy as np

    async def scenario() -> list[str]:
        shown: list[str] = []

        async def request(samples):
            return "partial text"

        async def preview(text):
            shown.append(text)

        live = LiveTranscription(request, preview, min_speech_frames=3, speculate_every_frames=5)
        live.speech_started()
        frame = np.zeros(512, dtype=np.float32)
        for _ in range(6):
            live.frame(0.9, frame)
        segment = live.current
        assert segment.confirmed
        assert live.speculative_due(segment)
        live.launch_speculative(segment)
        await segment.task
        assert not live.speculative_due(segment)    # no new speech: no duplicate request
        live.reset()
        return shown

    assert asyncio.run(scenario()) == ["partial text"]


def test_websocket_keeps_listening_and_orders_finals(tmp_path: Path, monkeypatch) -> None:
    """A slow first transcription must not stall intake or reorder results."""
    import numpy as np
    from voiceservice.api import sessions

    class AmplitudeVAD:
        def __init__(self, path):
            pass

        def probability(self, frame):
            return 0.9 if float(np.abs(frame).mean()) > 0.05 else 0.0

        def reset(self):
            pass

    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    (tmp_path / "runtime").mkdir()
    vad_file = tmp_path / "vad.onnx"
    vad_file.write_bytes(b"fake")
    cfg = tmp_path / "config.yaml"
    cfg.write_text(
        "backends:\n  audiocpp_clone:\n    enabled: false\n"
        f"sessions:\n  vad_model: {vad_file.as_posix()}\n  speculative: false\n"
        "  silence_frames: 4\n  min_speech_frames: 2\n",
        encoding="utf-8",
    )
    monkeypatch.setattr(sessions, "SileroVAD", AmplitudeVAD)
    release_first = asyncio.Event()
    order = []

    with TestClient(create_app(cfg), base_url="http://127.0.0.1") as client:
        registry = client.app.state.registry

        async def transcribe(wav, *, model, language=None, **kw):
            index = len(order)
            order.append(index)
            if index == 0:
                # Bounded: blocking intake would deliver this final before the
                # second speech_started, and the assertion below catches it.
                try:
                    await asyncio.wait_for(release_first.wait(), 5)
                except asyncio.TimeoutError:
                    pass
            return {"text": f"utterance {index}"}

        monkeypatch.setattr(registry.audiocpp, "transcribe", transcribe)
        speech = (np.full(512 * 8, 0.3, dtype=np.float32) * 32767).astype("<i2").tobytes()
        silence = np.zeros(512 * 8, dtype="<i2").tobytes()
        with client.websocket_connect("ws://127.0.0.1/v1/sessions/dictation") as ws:
            assert ws.receive_json()["type"] == "ready"
            ws.send_bytes(speech + silence)          # utterance 0 ends; its ASR blocks
            assert ws.receive_json()["type"] == "speech_started"
            ws.send_bytes(speech + silence)          # intake continues during that ASR
            assert ws.receive_json()["type"] == "speech_started"
            loop_events = []
            # unblock the first recognition from the server's event loop
            client.portal.call(release_first.set)
            ws.send_text('{"type":"finish"}')
            while True:
                event = ws.receive_json()
                loop_events.append(event)
                if event["type"] == "done":
                    break
    finals = [e["text"] for e in loop_events if e["type"] == "final"]
    assert finals == ["utterance 0", "utterance 1"]
