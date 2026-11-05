"""Synthesis bounds and per-model request options.

Both are production risks found by measurement: without a frame cap a
repetition loop produced 131 s of audio and held the request for 415 s, and a
model that does not declare a `language` option rejects the whole request when
the field is sent anyway.
"""
from __future__ import annotations

import asyncio
from pathlib import Path

import pytest

from voiceservice.api.openai import tts_max_tokens
from voiceservice.backends.audiocpp import AudioCppBackend


def test_tts_cap_has_a_floor_and_a_ceiling() -> None:
    assert tts_max_tokens("Hi") == 64                      # floor keeps short clips intact
    assert tts_max_tokens("x" * 4000) == 1024              # ceiling bounds a runaway
    assert 64 < tts_max_tokens("This is a normal sentence.") < 200


def test_tts_cap_scales_with_bytes_not_characters() -> None:
    cyrillic = tts_max_tokens("Это проверка русского текста.")
    latin = tts_max_tokens("This is a check of Russian text.")
    assert cyrillic > latin          # UTF-8 bytes, the same rule as the text cap


def test_tts_cap_honours_an_explicit_request() -> None:
    assert tts_max_tokens("Hi", 300) == 300
    assert tts_max_tokens("Hi", 99999) == 4096             # clamped, not trusted


# --------------------------------------------------------------------- backend
class _Lease:
    async def __aenter__(self) -> "_Lease":
        return self

    async def __aexit__(self, *exc) -> bool:
        return False


class _Response:
    status_code = 200
    content = b"RIFF-fake-wav"

    def json(self) -> dict:
        return {"text": "ok"}


def _backend(tmp_path: Path, models: list[dict]) -> AudioCppBackend:
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
        "models": models,
    }
    backend = AudioCppBackend(cfg, tmp_path / "state", tmp_path / "logs")
    backend.proc.lease = lambda: _Lease()          # no child process
    backend.proc.http = lambda: None               # the HTTP call is stubbed too
    return backend


def _capture(monkeypatch) -> dict:
    seen: dict = {}

    async def fake_post(client, url, **kwargs):
        seen.update(kwargs)
        seen["url"] = url
        return _Response()

    monkeypatch.setattr("voiceservice.backends.audiocpp._post_with_retry", fake_post)
    return seen


def test_speech_sends_a_frame_cap(tmp_path: Path, monkeypatch) -> None:
    backend = _backend(tmp_path, [{"id": "m", "family": "qwen3_tts", "task": "tts", "path": "x"}])
    seen = _capture(monkeypatch)
    asyncio.run(backend.speech("Hello there.", model="m", options={"max_tokens": 77}))
    assert seen["json"]["options"]["max_tokens"] == 77


def test_speech_omits_language_when_the_model_has_no_such_option(tmp_path: Path, monkeypatch) -> None:
    backend = _backend(
        tmp_path,
        [
            {"id": "moss", "family": "moss_tts_nano", "task": "tts", "path": "x", "supports_language": False},
            {"id": "qwen", "family": "qwen3_tts", "task": "tts", "path": "y"},
        ],
    )
    seen = _capture(monkeypatch)
    asyncio.run(backend.speech("Привет", model="moss", language="ru"))
    assert "language" not in seen["json"]

    asyncio.run(backend.speech("Привет", model="qwen", language="ru"))
    assert seen["json"]["language"] == "ru"


def test_transcription_omits_language_for_parakeet(tmp_path: Path, monkeypatch) -> None:
    backend = _backend(
        tmp_path,
        [
            {"id": "parakeet", "family": "parakeet_tdt", "task": "asr", "path": "x", "supports_language": False},
            {"id": "qwen3-asr", "family": "qwen3_asr", "task": "asr", "path": "y"},
        ],
    )
    seen = _capture(monkeypatch)
    asyncio.run(backend.transcribe(b"wav", model="parakeet", language="ru"))
    assert "language" not in seen["data"]      # the engine rejects the field outright

    asyncio.run(backend.transcribe(b"wav", model="qwen3-asr", language="ru"))
    assert seen["data"]["language"] == "ru"
