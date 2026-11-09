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
    engine = runtime / "bin/omnivoice-cpu/omnivoice-tts.exe"
    engine.parent.mkdir(parents=True)
    engine.write_bytes(b"")  # Existence-only fixture; never executed.
    config = tmp_path / "config.yaml"
    config.write_text("service:\n  host: 127.0.0.1\nbackends:\n  llama_translate:\n    enabled: false\n", encoding="utf-8")
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
    assert voice["model"] == "omnivoice-q8"   # default clone model

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

    async def fake_speech(text, *, model, voice=None, speed=None, language=None, options=None, timeout=300.0):
        calls.append({"text": text, "model": model, "voice": voice, "language": language})
        return _wav_bytes(1.0)

    class FakeBackend:
        speech = staticmethod(fake_speech)

    monkeypatch.setattr(client.app.state.registry, "tts_backend", lambda model_id: FakeBackend())

    resp = client.post("/v1/voices/preview-me/preview")
    assert resp.status_code == 200, resp.text
    assert resp.headers["content-type"].startswith("audio/wav")
    assert "x-generation-ms" in resp.headers
    assert calls and calls[0]["model"] == "omnivoice-q8"
    assert calls[0]["voice"] == "preview-me"
    assert calls[0]["language"] == "ru"


def test_speech_endpoint_routes_named_profile_to_clone_model(client: TestClient, monkeypatch) -> None:
    client.post(
        "/v1/voices",
        data={"name": "Routed", "language": "en", "reference_text": "hi"},
        files={"file": ("ref.wav", _wav_bytes(), "audio/wav")},
    )
    calls: list[dict] = []

    async def fake_speech(text, *, model, voice=None, speed=None, language=None, options=None, timeout=300.0):
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
    assert calls == [{"model": "omnivoice-q8", "voice": "routed", "language": "en"}]


def test_stale_profile_model_falls_back_to_configured_clone(client: TestClient, monkeypatch) -> None:
    """A profile recorded under a clone engine that is no longer configured (the
    audio.cpp qwen3-tts-clone helper after switching to Omnivoice) must keep
    working. The service resolves the stale id to the current clone engine
    instead of handing it to a backend that answers \"unknown model id\".
    """
    client.post(
        "/v1/voices",
        data={"name": "Legacy", "language": "en", "reference_text": "hi"},
        files={"file": ("ref.wav", _wav_bytes(), "audio/wav")},
    )
    registry = client.app.state.registry
    assert "qwen3-tts-clone" not in registry.tts_model_ids()   # engine is gone
    # Rewrite the stored profile as if it had been created under the old engine.
    client.app.state.voices.update("legacy", model="qwen3-tts-clone")
    assert client.app.state.voices.get("legacy")["model"] == "qwen3-tts-clone"

    calls: list[dict] = []

    async def fake_speech(text, *, model, voice=None, speed=None, language=None, options=None, timeout=300.0):
        calls.append({"model": model, "voice": voice, "language": language})
        return _wav_bytes(1.0)

    class FakeBackend:
        speech = staticmethod(fake_speech)

    monkeypatch.setattr(registry, "tts_backend", lambda model_id: FakeBackend())
    resp = client.post("/v1/audio/speech", json={"voice": "legacy", "input": "Hello"})
    assert resp.status_code == 200, resp.text
    assert calls == [{"model": "omnivoice-q8", "voice": "legacy", "language": "en"}]

    # The profile preview syntaxes through the same resolution path.
    resp = client.post("/v1/voices/legacy/preview")
    assert resp.status_code == 200, resp.text
    assert calls[-1] == {"model": "omnivoice-q8", "voice": "legacy", "language": "en"}


def test_tts_routing_picks_the_clone_engine(client: TestClient) -> None:
    registry = client.app.state.registry
    clone = registry.tts_backend("omnivoice-q8")
    assert clone is registry.omnivoice, "clone model must not run in the main CPU process"
    assert registry.tts_backend("kokoro") is registry.audiocpp
    assert registry.tts_model_ids().count("omnivoice-q8") == 1


def test_output_language_follows_text_not_reference_language(client: TestClient, monkeypatch) -> None:
    client.post(
        "/v1/voices", data={"name": "Russian reference", "language": "ru", "reference_text": "Пример"},
        files={"file": ("ref.wav", _wav_bytes(), "audio/wav")},
    )
    languages = []

    async def fake_speech(text, *, model, voice=None, speed=None, language=None, options=None, timeout=300.0):
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

    async def fake_speech(text, *, model, voice=None, speed=None, language=None, options=None, timeout=300.0):
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
    assert caps.get("omnivoice-q8") == "tts"
    assert caps.get("nemotron") == "asr"


def test_server_voice_defaults_and_explicit_overrides(client: TestClient, monkeypatch) -> None:
    for name in ("English default", "Russian default", "Override"):
        response = client.post(
            "/v1/voices", data={"name": name, "reference_text": "sample"},
            files={"file": ("ref.wav", _wav_bytes(), "audio/wav")},
        )
        assert response.status_code == 200
    registry = client.app.state.registry
    registry.cfg["tts"]["default_voices"] = {"en": "english-default", "ru": "russian-default"}
    calls = []

    async def fake_speech(text, *, model, voice=None, speed=None, language=None, options=None, timeout=300.0):
        calls.append((model, voice, language))
        return _wav_bytes(1.0)

    class FakeBackend:
        speech = staticmethod(fake_speech)

    monkeypatch.setattr(registry, "tts_backend", lambda model: FakeBackend())
    for payload in (
        {"input": "Hello"},
        {"input": "Привет"},
        {"input": "Привет", "voice": "override"},
        {"input": "Hello", "model": "kokoro"},
    ):
        assert client.post("/v1/audio/speech", json=payload).status_code == 200
    assert calls == [
        ("omnivoice-q8", "english-default", "en"),
        ("omnivoice-q8", "russian-default", "ru"),
        ("omnivoice-q8", "override", "ru"),
        ("kokoro", None, None),
    ]


def test_missing_configured_voice_fails_clearly(client: TestClient) -> None:
    client.app.state.registry.cfg["tts"]["default_voices"]["ru"] = "deleted-profile"
    response = client.post("/v1/audio/speech", json={"input": "Привет"})
    assert response.status_code == 503
    assert "deleted-profile" in response.json()["detail"]


def test_default_voice_may_be_a_builtin_model(client: TestClient, monkeypatch) -> None:
    """tts.default_voices.<lang> can name a built-in model (male/female choice)."""
    calls: list[tuple[str, str | None]] = []

    async def fake_speech(text, *, model, voice=None, speed=None, language=None, options=None, timeout=300.0):
        calls.append((model, voice))
        return _wav_bytes(1.0)

    class FakeBackend:
        speech = staticmethod(fake_speech)

    registry = client.app.state.registry
    monkeypatch.setattr(registry, "tts_backend", lambda model: FakeBackend())
    registry.cfg["tts"]["default_voices"]["ru"] = "piper-ru-female"
    assert client.post("/v1/audio/speech", json={"input": "Привет"}).status_code == 200
    assert calls == [("piper-ru-female", None)]
    # An explicit voice profile still wins over the configured default.
    client.post(
        "/v1/voices", data={"name": "Override 2", "reference_text": "sample"},
        files={"file": ("ref.wav", _wav_bytes(), "audio/wav")},
    )
    assert client.post("/v1/audio/speech", json={"input": "Привет", "voice": "override-2"}).status_code == 200
    assert calls[-1][1] == "override-2"
