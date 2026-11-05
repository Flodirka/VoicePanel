"""TTS auto-routing: small per-language built-ins win over the clone model.

Kokoro (en) and piper-ru (ru) live in the main CPU process, so everyday speech
never loads the 2 GB clone model. The clone model is still returned for Russian
when no RU built-in is configured (it then needs a voice profile, enforced by
the speech endpoint), and for any explicit model request.
"""
from __future__ import annotations

from pathlib import Path

from voiceservice.backends import Registry


def _registry(tmp_path: Path, with_piper: bool = True) -> Registry:
    (tmp_path / "runtime").mkdir(exist_ok=True)
    for rel in ("b/a.exe", "b/c.exe"):
        p = tmp_path / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"MZ")
    models = [
        {"id": "kokoro", "family": "kokoro_tts", "path": "m/kokoro.gguf", "task": "tts",
         "mode": "offline", "voice": "af_heart", "language": "en"},
    ]
    if with_piper:
        models.append({"id": "piper-ru", "family": "piper_tts", "path": "m/piper.gguf",
                       "task": "tts", "mode": "offline", "language": "ru", "supports_language": False})
    clone = [
        {"id": "qwen3-tts-clone", "family": "qwen3_tts", "path": "m/clone.gguf", "task": "tts",
         "mode": "offline"},
    ]
    cfg = {
        "_root": str(tmp_path),
        "asr": {"default": "nemotron"},
        "tts": {"default": "auto", "clone_model": "qwen3-tts-clone", "default_voices": {"en": "", "ru": ""}},
        "llm": {"enabled": False},
        "backends": {
            "audiocpp": {"binary": str(tmp_path / "b/a.exe"), "port": 8101, "models": models + [
                {"id": "nemotron", "family": "nemotron_asr", "path": "m/asr.gguf", "task": "asr", "mode": "offline"}]},
            "audiocpp_clone": {"enabled": True, "binary": str(tmp_path / "b/c.exe"), "port": 8104, "models": clone},
            "audiocpp_diar": {"enabled": False},
            "whisper": {"enabled": False},
            "llama": {},
        },
    }
    return Registry(cfg, tmp_path / "state", tmp_path / "logs")


def test_auto_routes_cyrillic_to_piper_and_latin_to_kokoro(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    assert reg.resolve_tts("auto", "Это русский текст.") == "piper-ru"
    assert reg.resolve_tts(None, "This is English text.") == "kokoro"
    assert reg.resolve_tts("", "Привет") == "piper-ru"


def test_explicit_model_requests_are_honored(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    assert reg.resolve_tts("qwen3-tts-clone", "Hello") == "qwen3-tts-clone"
    assert reg.resolve_tts("piper-ru", "Hello") == "piper-ru"


def test_without_piper_cyrillic_falls_back_to_clone(tmp_path: Path) -> None:
    reg = _registry(tmp_path, with_piper=False)
    assert reg.resolve_tts("auto", "Это русский текст.") == "qwen3-tts-clone"
    assert reg.resolve_tts("auto", "English text.") == "kokoro"


def test_clone_model_routes_to_the_clone_backend(tmp_path: Path) -> None:
    reg = _registry(tmp_path)
    assert reg.tts_backend("qwen3-tts-clone") is reg.audiocpp_clone
    assert reg.tts_backend("piper-ru") is reg.audiocpp
    assert reg.tts_backend("kokoro") is reg.audiocpp