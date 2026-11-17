from pathlib import Path

from fastapi.testclient import TestClient

from voiceservice.app import create_app
from voiceservice.config import load_config


def test_engine_choice_is_saved_for_next_start_and_validated(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    (tmp_path / "runtime").mkdir()
    config = tmp_path / "config.yaml"
    config.write_text("backends:\n  llama_translate:\n    enabled: false\n", encoding="utf-8")
    cfg = load_config(config)
    for name in ("audiocpp_clone", "omnivoice"):
        section = cfg["backends"][name]
        for value in [section["binary"], *[m["path"] for m in section["models"]]]:
            p = Path(value)
            p.parent.mkdir(parents=True, exist_ok=True)
            p.touch()
    app = create_app(config)
    with TestClient(app, base_url="http://127.0.0.1") as client:
        response = client.post("/admin/engines", json={"tts": "audiocpp"})
        assert response.status_code == 200, response.text
        assert response.json()["restart_required"] is True
        assert app.state.cfg["backends"]["omnivoice"]["enabled"] is True
        assert client.post("/admin/engines", json={"tts": "unknown"}).status_code == 400
    reloaded = load_config(config)
    assert reloaded["backends"]["omnivoice"]["enabled"] is False
    assert reloaded["backends"]["audiocpp_clone"]["enabled"] is True
    assert reloaded["tts"]["clone_model"] == "qwen3-tts-clone"


def test_builtin_audio_speech_does_not_require_optional_cloning_weights(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    (tmp_path / "runtime").mkdir()
    config = tmp_path / "config.yaml"
    config.write_text("backends:\n  llama_translate:\n    enabled: false\n", encoding="utf-8")
    cfg = load_config(config)
    section = cfg["backends"]["audiocpp"]
    for value in (section["binary"], next(m["path"] for m in section["models"] if m["id"] == "piper-ru")):
        p = Path(value)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.touch()
    app = create_app(config)
    with TestClient(app, base_url="http://127.0.0.1") as client:
        response = client.post("/admin/engines", json={"tts": "audiocpp"})
        assert response.status_code == 200, response.text
    cfg = load_config(config)
    assert cfg["backends"]["omnivoice"]["enabled"] is False
    assert cfg["backends"]["audiocpp_clone"]["enabled"] is False
    assert cfg["tts"]["clone_model"] == ""
    gpu = tmp_path / "runtime/bin/llama-cuda/llama-server.exe"
    gpu.parent.mkdir(parents=True)
    gpu.touch()
    (tmp_path / "runtime/state/compute.json").write_text('{"clone":"gpu","llm":"gpu"}')
    app = create_app(config)
    with TestClient(app, base_url="http://127.0.0.1") as client:
        assert app.state.cfg["backends"]["llama"]["gpu_layers"] > 0
        assert "clone" not in {g["id"] for g in client.get("/admin/compute").json()["groups"]}
