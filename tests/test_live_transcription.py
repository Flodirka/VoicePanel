"""Live streaming transcription: the WebSocket relay and the engine client.

Live audio cannot travel through an ASGI request body: measured on this build,
uvicorn hands chunked ``http.request`` messages to a raw application (9 messages
for 25 KB) but FastAPI's ``request.stream()`` yields nothing once a
StreamingResponse has started, so the engine received an empty body and answered
"Nemotron ASR finalize requires streamed audio". The relay is therefore a
WebSocket, and the engine client is exercised against a real fake engine.
"""
from __future__ import annotations

import asyncio
import json
import socket
import threading
import time
from pathlib import Path

import pytest
import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse, StreamingResponse
from fastapi.testclient import TestClient

from voiceservice.app import create_app
from voiceservice.backends.audiocpp import AudioCppBackend
from voiceservice.config import load_config


@pytest.fixture()
def client(tmp_path: Path, monkeypatch) -> TestClient:
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    config = tmp_path / "config.yaml"
    config.write_text("service:\n  host: 127.0.0.1\nbackends:\n  llama_translate:\n    enabled: false\n", encoding="utf-8")
    app = create_app(config)
    with TestClient(app, base_url="http://127.0.0.1") as c:
        yield c


def _registry(client: TestClient):
    return client.app.state.registry


# ------------------------------------------------------------------ websocket
def test_stream_refuses_a_model_without_streaming_mode(client: TestClient) -> None:
    registry = _registry(client)
    registry.audiocpp.models["nemotron"]["mode"] = "offline"
    with client.websocket_connect("ws://127.0.0.1/v1/sessions/stream?model=nemotron") as ws:
        first = ws.receive_json()
        assert first["type"] == "error"
        assert "mode: streaming" in first["message"]


def test_stream_refuses_an_unknown_model(client: TestClient) -> None:
    with client.websocket_connect("ws://127.0.0.1/v1/sessions/stream?model=nope") as ws:
        assert ws.receive_json()["type"] == "error"


def test_stream_relays_pcm_and_events(client: TestClient) -> None:
    registry = _registry(client)
    registry.audiocpp.models["nemotron"]["mode"] = "streaming"
    seen = {"bytes": 0, "model": None, "language": None}

    async def fake_live(chunks, *, model, language=None, sample_rate=16000, channels=1):
        seen["model"] = model
        seen["language"] = language
        async for chunk in chunks:
            seen["bytes"] += len(chunk)
        yield 'data: {"type":"transcript.text.delta","delta":"Привет"}'
        yield 'data: {"type":"transcript.text.done","text":"Привет, мир."}'
        yield "data: [DONE]"

    registry.audiocpp.transcribe_live = fake_live  # type: ignore[method-assign]

    with client.websocket_connect("ws://127.0.0.1/v1/sessions/stream?model=nemotron&language=ru-RU") as ws:
        assert ws.receive_json() == {"type": "ready", "model": "nemotron", "sample_rate": 16000}
        ws.send_bytes(b"\x01\x02" * 800)
        ws.send_bytes(b"\x03\x04" * 800)
        ws.send_json({"type": "finish"})
        events = []
        while True:
            event = ws.receive_json()
            events.append(event)
            if event["type"] in ("done", "error"):
                break

    assert seen["bytes"] == 3200          # every frame reached the engine
    assert seen["model"] == "nemotron"
    assert seen["language"] == "ru-RU"
    assert [e["type"] for e in events] == [
        "transcript.text.delta",
        "transcript.text.done",
        "done",
    ]
    assert events[1]["text"] == "Привет, мир."


def test_stream_reports_an_engine_failure_without_dropping_the_socket(client: TestClient) -> None:
    registry = _registry(client)
    registry.audiocpp.models["nemotron"]["mode"] = "streaming"

    async def failing_live(chunks, **kwargs):
        async for _ in chunks:
            pass
        raise RuntimeError("engine said no")
        yield  # pragma: no cover

    registry.audiocpp.transcribe_live = failing_live  # type: ignore[method-assign]
    with client.websocket_connect("ws://127.0.0.1/v1/sessions/stream?model=nemotron") as ws:
        assert ws.receive_json()["type"] == "ready"
        ws.send_bytes(b"\x00\x00" * 16)
        ws.send_json({"type": "finish"})
        event = ws.receive_json()
        assert event["type"] == "error"
        assert "engine said no" in event["message"]


# ------------------------------------------------------------------- client
def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _serve(app: FastAPI) -> tuple[uvicorn.Server, int]:
    port = _free_port()
    server = uvicorn.Server(uvicorn.Config(app, host="127.0.0.1", port=port, log_level="error"))
    threading.Thread(target=server.run, daemon=True).start()
    deadline = time.monotonic() + 10
    while not server.started and time.monotonic() < deadline:
        time.sleep(0.05)
    return server, port


def test_transcribe_live_streams_pcm_and_reads_events(tmp_path: Path) -> None:
    received: list[bytes] = []
    query: dict = {}
    fake = FastAPI()

    @fake.post("/v1/audio/transcriptions/live")
    async def live(request: Request):
        query.update(dict(request.query_params))
        async for chunk in request.stream():
            received.append(chunk)

        async def events():
            yield b'data: {"type":"transcript.text.done","text":"ok"}\n\n'
            yield b"data: [DONE]\n\n"

        return StreamingResponse(events(), media_type="text/event-stream")

    server, port = _serve(fake)
    cfg = {**load_config(None)["backends"]["audiocpp"], "port": port}
    backend = AudioCppBackend(cfg, tmp_path, tmp_path / "logs")
    backend.proc._owned_and_healthy = lambda: True  # type: ignore[method-assign]

    async def chunks():
        yield b"\x01\x02" * 800
        yield b"\x03\x04" * 800

    async def scenario() -> list[str]:
        lines = []
        async for line in backend.transcribe_live(
            chunks(), model="nemotron", language="ru-RU", sample_rate=16000
        ):
            lines.append(line)
        await backend.proc._close_http()
        return lines

    try:
        lines = asyncio.run(scenario())
    finally:
        server.should_exit = True

    assert sum(len(c) for c in received) == 3200
    assert query["model"] == "nemotron"
    assert query["sample_rate"] == "16000"
    assert query["language"] == "ru-RU"
    assert any("transcript.text.done" in line for line in lines)
    assert any("[DONE]" in line for line in lines)


def test_transcribe_live_surfaces_engine_errors(tmp_path: Path) -> None:
    fake = FastAPI()

    @fake.post("/v1/audio/transcriptions/live")
    async def live():
        return JSONResponse({"error": {"message": "no streaming session"}}, status_code=400)

    server, port = _serve(fake)
    cfg = {**load_config(None)["backends"]["audiocpp"], "port": port}
    backend = AudioCppBackend(cfg, tmp_path, tmp_path / "logs")
    backend.proc._owned_and_healthy = lambda: True  # type: ignore[method-assign]

    async def chunks():
        yield b"\x00\x00" * 16

    async def scenario() -> None:
        async for _ in backend.transcribe_live(chunks(), model="nemotron"):
            pass
        await backend.proc._close_http()

    try:
        with pytest.raises(RuntimeError) as excinfo:
            asyncio.run(scenario())
    finally:
        server.should_exit = True
    assert "live transcription failed" in str(excinfo.value)
