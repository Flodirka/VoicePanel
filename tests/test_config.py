"""Configuration: cloning defaults, user overrides, relative path resolution."""
from __future__ import annotations

from pathlib import Path

from voiceservice.config import load_config


def test_defaults_include_cloning_support() -> None:
    cfg = load_config(None)
    assert cfg["tts"]["clone_model"] == "qwen3-tts-clone"
    clone_cfg = cfg["backends"]["audiocpp_clone"]
    models = {m["id"]: m for m in clone_cfg["models"]}
    assert "qwen3-tts-clone" in models
    assert models["qwen3-tts-clone"]["session_options"]["qwen3_tts.voice_prompt_cache_slots"] == "4"
    assert cfg["backends"]["audiocpp"]["voice_dir"].endswith("voice-library")
    # The main process stays CPU-first: the clone model lives in the GPU helper.
    assert cfg["backends"]["audiocpp"]["backend"] == "cpu"
    assert "qwen3-tts-clone" not in [m["id"] for m in cfg["backends"]["audiocpp"]["models"]]


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
    assert cfg["tts"]["clone_model"] == "qwen3-tts-clone"      # defaults survive the merge
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
