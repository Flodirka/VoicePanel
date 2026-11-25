"""Recognition context must reach the backend in every response format.

A verbose_json request used to be routed to the detail variant, which took no
prompt, so the glossary was dropped without a word on the way back. These tests
fail if either format loses it again.
"""
from __future__ import annotations

import asyncio
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from voiceservice.app import create_app
from voiceservice.backends.audiocpp import AudioCppBackend

PROMPT = "UCLASS, VoicePanel, Zhora"


@pytest.fixture()
def service(tmp_path: Path, monkeypatch) -> TestClient:
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    (tmp_path / "runtime").mkdir()
    cfg = tmp_path / "config.yaml"
    cfg.write_text(
        "llm:\n  skip_if_clean: false\nbackends:\n"
        "  audiocpp_clone:\n    enabled: false\n  llama_translate:\n    enabled: false\n",
        encoding="utf-8",
    )
    with TestClient(create_app(cfg), base_url="http://127.0.0.1") as client:
        yield client


def test_json_transcription_passes_the_prompt_to_the_backend(service, monkeypatch) -> None:
    seen: dict = {}

    async def transcribe(wav, *, model, **kw):
        seen.update(kw)
        return {"text": "words"}

    monkeypatch.setattr(service.app.state.registry.audiocpp, "transcribe", transcribe)
    resp = service.post(
        "/v1/audio/transcriptions",
        files={"file": ("a.wav", _wav(), "audio/wav")},
        data={"prompt": PROMPT, "response_format": "json"},
    )
    assert resp.status_code == 200, resp.text
    assert seen.get("prompt") == PROMPT


def test_verbose_transcription_passes_the_prompt_to_the_backend(service, monkeypatch) -> None:
    seen: dict = {}

    async def transcribe_detailed(wav, *, model, **kw):
        seen.update(kw)
        return {"text": "words", "segments": [{"start": 0.0, "end": 1.0, "text": "words"}]}

    monkeypatch.setattr(
        service.app.state.registry.audiocpp, "transcribe_detailed", transcribe_detailed
    )
    resp = service.post(
        "/v1/audio/transcriptions",
        files={"file": ("a.wav", _wav(), "audio/wav")},
        data={"prompt": PROMPT, "response_format": "verbose_json"},
    )
    assert resp.status_code == 200, resp.text
    assert seen.get("prompt") == PROMPT


class _Lease:
    async def __aenter__(self) -> None:
        return None

    async def __aexit__(self, *exc) -> bool:
        return False


class _Response:
    status_code = 200
    content = b""

    def json(self) -> dict:
        return {"text": "ok"}


def _backend(tmp_path: Path) -> AudioCppBackend:
    cfg = {
        "binary": str(tmp_path / "audiocpp_server.exe"),
        "backend": "cpu",
        "device": 0,
        "threads": 4,
        "host": "127.0.0.1",
        "port": 8101,
        "lazy_load": True,
        "idle_unload_ms": 1000,
        "max_loaded_models": 1,
        "start_timeout_s": 5,
        "idle_stop_s": 100,
        "models": [{"id": "nemotron", "family": "nemotron_asr", "task": "asr", "path": "x"}],
    }
    backend = AudioCppBackend(cfg, tmp_path / "state", tmp_path / "logs")
    backend.proc.lease = lambda: _Lease()      # no child process
    backend.proc.http = lambda: None           # the HTTP call is stubbed
    return backend


def test_detail_variant_sends_the_prompt_field(tmp_path: Path, monkeypatch) -> None:
    seen: dict = {}

    async def fake_post(client, url, **kwargs):
        seen.update(kwargs)
        seen["url"] = url
        return _Response()

    monkeypatch.setattr("voiceservice.backends.audiocpp._post_with_retry", fake_post)
    backend = _backend(tmp_path)
    asyncio.run(backend.transcribe_detailed(b"wav", model="nemotron", language="en", prompt=PROMPT))
    assert seen["url"].endswith("/v1/audio/transcriptions/details")
    assert seen["data"]["prompt"] == PROMPT


def test_detail_variant_omits_an_empty_prompt(tmp_path: Path, monkeypatch) -> None:
    seen: dict = {}

    async def fake_post(client, url, **kwargs):
        seen.update(kwargs)
        return _Response()

    monkeypatch.setattr("voiceservice.backends.audiocpp._post_with_retry", fake_post)
    backend = _backend(tmp_path)
    asyncio.run(backend.transcribe_detailed(b"wav", model="nemotron"))
    assert "prompt" not in seen["data"]      # an empty field is not context


def _wav() -> bytes:
    """A short 16 kHz WAV, encoded here so the test needs no fixture file."""
    import struct

    samples = [0] * 1600
    data = struct.pack(f"<{len(samples)}h", *samples)
    header = b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt "
    header += struct.pack("<IHHIIHH", 16, 1, 1, 16000, 32000, 2, 16)
    header += b"data" + struct.pack("<I", len(data))
    return header + data
