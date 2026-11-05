"""Configuration: cloning defaults, user overrides, relative path resolution."""
from __future__ import annotations

from pathlib import Path

from voiceservice.config import load_config


def test_defaults_are_the_minimal_small_model_set() -> None:
    # The base configuration is the minimal small-model stack verified in
    # docs/alternative-runtimes.md: nemotron ASR, GEC corrector, PLaMo
    # translator, kokoro/piper built-in voices, omnivoice.cpp for cloning.
    cfg = load_config(None)
    assert cfg["asr"]["default"] == "nemotron"
    audiocpp = {m["id"]: m for m in cfg["backends"]["audiocpp"]["models"]}
    assert audiocpp["nemotron"]["family"] == "nemotron_asr"
    assert audiocpp["kokoro"]["family"] == "kokoro_tts"
    assert audiocpp["piper-ru"]["family"] == "piper_tts"
    assert audiocpp["piper-ru-female"]["family"] == "piper_tts"
    assert cfg["backends"]["llama"]["model_alias"] == "gec-0.8b"
    tr = cfg["backends"]["llama_translate"]
    assert tr["enabled"] and tr["model_alias"] == "plamo-3-translate-0.8b"
    assert tr["template_style"] == "plamo"
    assert "qwen3-tts-clone" in {m["id"] for m in cfg["backends"]["audiocpp_clone"]["models"]}
    assert cfg["backends"]["audiocpp_clone"]["enabled"] is False   # the alternative
    assert cfg["llm"]["skip_if_clean"] is True


def test_defaults_include_cloning_support() -> None:
    cfg = load_config(None)
    assert cfg["tts"]["clone_model"] == "omnivoice-q8"
    clone_cfg = cfg["backends"]["omnivoice"]
    assert clone_cfg["enabled"] is True
    models = {m["id"]: m for m in clone_cfg["models"]}
    models["omnivoice-q8"]["codec_path"].endswith("omnivoice-tokenizer-Q8_0.gguf")
    assert models["omnivoice-q8"]["path"].endswith("omnivoice-base-Q8_0.gguf")
    assert cfg["backends"]["audiocpp"]["voice_dir"].endswith("voice-library")
    # The main process stays CPU-first: the clone engine is its own process.
    assert cfg["backends"]["audiocpp"]["backend"] == "cpu"
    assert "omnivoice-q8" not in [m["id"] for m in cfg["backends"]["audiocpp"]["models"]]


def test_user_config_overrides_and_lists_replace(tmp_path: Path) -> None:
    cfg_path = tmp_path / "config.yaml"
    cfg_path.write_text(
        "\n".join(
            [
                "service:",
                "  port: 9099",
                "tts:",
                "  default: piper-ru",
                "backends:",
                "  audiocpp:",
                "    backend: cpu",
                "    models:",
                "      - id: only-asr",
                "        family: qwen3_asr",
                "        path: runtime/models/x.gguf",
                "        task: asr",
            ]
        ),
        encoding="utf-8",
    )
    cfg = load_config(cfg_path)

    assert cfg["service"]["port"] == 9099
    assert cfg["tts"]["default"] == "piper-ru"
    assert cfg["tts"]["clone_model"] == "omnivoice-q8"          # defaults survive the merge
    assert cfg["backends"]["audiocpp"]["backend"] == "cpu"
    assert [m["id"] for m in cfg["backends"]["audiocpp"]["models"]] == ["only-asr"]


def test_relative_paths_resolve_against_repo_root(tmp_path: Path) -> None:
    (tmp_path / "runtime").mkdir()
    cfg_path = tmp_path / "config.yaml"
    cfg_path.write_text("backends:\n  audiocpp:\n    voice_dir: runtime/my-voices\n", encoding="utf-8")

    cfg = load_config(cfg_path)
    assert Path(cfg["backends"]["audiocpp"]["voice_dir"]) == tmp_path / "runtime" / "my-voices"


def test_env_expansion(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("VP_TEST_DIR", str(tmp_path / "voices"))
    cfg_path = tmp_path / "config.yaml"
    cfg_path.write_text("backends:\n  audiocpp:\n    voice_dir: ${VP_TEST_DIR}\n", encoding="utf-8")

    cfg = load_config(cfg_path)
    assert cfg["backends"]["audiocpp"]["voice_dir"] == str(tmp_path / "voices")
