"""HTTP API: voice profiles CRUD, preview, and speech routing to a cloned voice."""
from __future__ import annotations

import io
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from voiceservice.app import create_app
from voiceservice.voices import VoiceStore


@pytest.fixture()
def client(tmp_path: Path, monkeypatch) -> TestClient:
    """App with a private voice library; no backend processes are started."""
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    runtime = tmp_path / "runtime"
    helper = runtime / "bin/audiocpp-cuda/audiocpp_server.exe"
    helper.parent.mkdir(parents=True)
    helper.write_bytes(b"")  # Existence-only fixture; never executed.
    config = tmp_path / "config.yaml"
    config.write_text("service:\n  host: 127.0.0.1\n", encoding="utf-8")
    app = create_app(config)
    with TestClient(app, base_url="http://127.0.0.1") as c:
        app.state.voices = VoiceStore(tmp_path / "voice-library")
        yield c


def _wav_bytes(seconds: float = 3.0) -> bytes:
    from conftest import make_wav

    return make_wav(seconds)


def test_create_list_get_delete(client: TestClient) -> None:
    resp = client.post(
        "/v1/voices",
        data={"name": "Panel Voice", "language": "en", "reference_text": "hello"},
        files={"file": ("ref.wav", _wav_bytes(), "audio/wav")},
    )
    assert resp.status_code == 200, resp.text
    voice = resp.json()["voice"]
    assert voice["id"] == "panel-voice"
    assert voice["model"] == "qwen3-tts-clone"   # default clone model

    listing = client.get("/v1/voices").json()["voices"]
    assert [v["id"] for v in listing] == ["panel-voice"]
    assert client.get("/v1/voices/panel-voice").status_code == 200

    patched = client.patch(
        "/v1/voices/panel-voice",
        data={"name": "Renamed", "reference_text": "hello there"},
    )
    assert patched.status_code == 200, patched.text
    assert patched.json()["voice"]["name"] == "Renamed"
    assert patched.json()["voice"]["reference_text"] == "hello there"

    assert client.delete("/v1/voices/panel-voice").status_code == 200
    assert client.get("/v1/voices/panel-voice").status_code == 404


def test_create_rejects_short_reference(client: TestClient) -> None:
    resp = client.post(
        "/v1/voices",
        data={"name": "Short"},
        files={"file": ("ref.wav", _wav_bytes(0.5), "audio/wav")},
    )
    assert resp.status_code == 400
    assert "too short" in resp.json()["detail"]


def test_preview_synthesises_with_the_cloning_model(client: TestClient, monkeypatch) -> None:
    client.post(
        "/v1/voices",
        data={"name": "Preview Me", "language": "ru", "reference_text": "тест"},
        files={"file": ("ref.wav", _wav_bytes(), "audio/wav")},
    )
    calls: list[dict] = []

    async def fake_speech(text, *, model, voice=None, speed=None, language=None, timeout=300.0):
        calls.append({"text": text, "model": model, "voice": voice, "language": language})
        return _wav_bytes(1.0)

    class FakeBackend:
        speech = staticmethod(fake_speech)

    monkeypatch.setattr(client.app.state.registry, "tts_backend", lambda model_id: FakeBackend())

    resp = client.post("/v1/voices/preview-me/preview")
    assert resp.status_code == 200, resp.text
    assert resp.headers["content-type"].startswith("audio/wav")
    assert "x-generation-ms" in resp.headers
    assert calls and calls[0]["model"] == "qwen3-tts-clone"
    assert calls[0]["voice"] == "preview-me"
    assert calls[0]["language"] == "ru"


def test_speech_endpoint_routes_named_profile_to_clone_model(client: TestClient, monkeypatch) -> None:
    client.post(
        "/v1/voices",
        data={"name": "Routed", "language": "en", "reference_text": "hi"},
        files={"file": ("ref.wav", _wav_bytes(), "audio/wav")},
    )
    calls: list[dict] = []

    async def fake_speech(text, *, model, voice=None, speed=None, language=None, timeout=300.0):
        calls.append({"model": model, "voice": voice, "language": language})
        return _wav_bytes(1.0)

    class FakeBackend:
        speech = staticmethod(fake_speech)

    monkeypatch.setattr(client.app.state.registry, "tts_backend", lambda model_id: FakeBackend())

    resp = client.post(
        "/v1/audio/speech",
        json={"voice": "routed", "input": "Hello there", "model": "profile"},
    )
    assert resp.status_code == 200, resp.text
    assert calls == [{"model": "qwen3-tts-clone", "voice": "routed", "language": "en"}]


def test_tts_routing_picks_the_clone_helper(client: TestClient) -> None:
    registry = client.app.state.registry
    clone = registry.tts_backend("qwen3-tts-clone")
    assert clone is registry.audiocpp_clone, "clone model must not run in the main CPU process"
    assert registry.tts_backend("kokoro") is registry.audiocpp
    assert registry.tts_model_ids().count("qwen3-tts-clone") == 1


def test_output_language_follows_text_not_reference_language(client: TestClient, monkeypatch) -> None:
    client.post(
        "/v1/voices", data={"name": "Russian reference", "language": "ru", "reference_text": "Пример"},
        files={"file": ("ref.wav", _wav_bytes(), "audio/wav")},
    )
    languages = []

    async def fake_speech(text, *, model, voice=None, speed=None, language=None, timeout=300.0):
        languages.append(language)
        return _wav_bytes(1.0)

    class FakeBackend:
        speech = staticmethod(fake_speech)

    monkeypatch.setattr(client.app.state.registry, "tts_backend", lambda model_id: FakeBackend())
    response = client.post("/v1/audio/speech", json={"voice": "russian-reference", "input": "Hello in English."})
    assert response.status_code == 200
    assert languages == ["en"]


def test_speech_endpoint_unknown_voice_shape_falls_back_to_model(client: TestClient, monkeypatch) -> None:
    calls: list[dict] = []

    async def fake_speech(text, *, model, voice=None, speed=None, language=None, timeout=300.0):
        calls.append({"model": model, "voice": voice})
        return _wav_bytes(1.0)

    class FakeBackend:
        speech = staticmethod(fake_speech)

    monkeypatch.setattr(client.app.state.registry, "tts_backend", lambda model_id: FakeBackend())

    # "af_heart" is a built-in preset, not a profile: the requested model wins.
    resp = client.post(
        "/v1/audio/speech",
        json={"voice": "af_heart", "input": "Hi", "model": "kokoro"},
    )
    assert resp.status_code == 200, resp.text
    assert calls == [{"model": "kokoro", "voice": "af_heart"}]


def test_models_lists_clone_model(client: TestClient) -> None:
    data = client.get("/v1/models").json()["data"]
    caps = {m["id"]: m["capability"] for m in data}
    assert caps.get("qwen3-tts-clone") == "tts"
    assert caps.get("qwen3-asr") == "asr"
