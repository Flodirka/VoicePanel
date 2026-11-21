"""Backend choices persisted separately from the user's model configuration."""
from __future__ import annotations

import copy
import json
import logging
from pathlib import Path
from typing import Any

from .tts import speech_parameters


def state_path(cfg: dict) -> Path:
    return Path(cfg["_root"]) / "runtime/state/engines.json"


def current(cfg: dict) -> dict[str, str]:
    return {
        "tts": "omnivoice" if cfg["backends"].get("omnivoice", {}).get("enabled", False) else "audiocpp",
        "asr": "whisper" if cfg.get("asr", {}).get("default") == "whisper" else "audiocpp",
    }


def apply(cfg: dict, choices: dict[str, str]) -> None:
    for task, engine in choices.items():
        if task not in ("tts", "asr") or engine not in (("omnivoice", "audiocpp") if task == "tts" else ("audiocpp", "whisper")):
            raise ValueError(f"unsupported {task} engine '{engine}'")
        backends = cfg["backends"]
        if task == "tts":
            section = backends["omnivoice" if engine == "omnivoice" else "audiocpp_clone"]
            models = [m for m in section.get("models", []) if m.get("task") == "tts"]
            if engine == "audiocpp":
                ready = Path(section.get("binary", "")).is_file() and any(Path(m.get("path", "")).is_file() for m in models)
                if not ready:
                    section = backends["audiocpp"]
                    models = [m for m in section.get("models", []) if m.get("task") == "tts" and Path(m.get("path", "")).is_file()]
            if not models:
                raise ValueError(f"no speech model is configured for {engine}")
            model = next((m for m in models if m["id"] == cfg["tts"].get("clone_model")), models[0])
            paths = [section.get("binary"), model.get("path")]
            if engine == "omnivoice":
                paths += [section.get("codec_binary"), model.get("codec_path")]
            if any(not p or not Path(p).is_file() for p in paths):
                raise ValueError(f"install the {engine} binaries and configured model files first")
            backends["omnivoice"]["enabled"] = engine == "omnivoice"
            backends["audiocpp_clone"]["enabled"] = engine == "audiocpp" and section is backends["audiocpp_clone"]
            cfg["tts"]["clone_model"] = model["id"] if speech_parameters(engine, model)["voice_cloning"] else ""
            cfg["tts"]["default"] = model["id"] if engine == "omnivoice" else "auto"
        else:
            section = backends["whisper" if engine == "whisper" else "audiocpp"]
            if engine == "whisper":
                model_path = section.get("model_path")
                model_id = "whisper"
            else:
                models = [m for m in section.get("models", []) if m.get("task") == "asr"]
                if not models:
                    raise ValueError("no audio.cpp recognition model is configured")
                model = next((m for m in models if m["id"] == cfg["asr"].get("default")), models[0])
                model_path, model_id = model.get("path"), model["id"]
            if any(not p or not Path(p).is_file() for p in (section.get("binary"), model_path)):
                raise ValueError(f"install the {engine} binaries and configured model files first")
            if engine == "whisper":
                section["enabled"] = True
            cfg["asr"]["default"] = model_id


def load_saved(cfg: dict) -> dict[str, str]:
    path = state_path(cfg)
    if not path.is_file():
        return {}
    try:
        choices = json.loads(path.read_text(encoding="utf-8"))
        candidate = copy.deepcopy(cfg)
        apply(candidate, choices)
        cfg.update(candidate)
        return choices
    except (OSError, ValueError, TypeError, AttributeError) as exc:
        logging.getLogger("voiceservice.engines").warning("ignoring unavailable engine choice: %s", exc)
        return {}


def save(cfg: dict, choices: dict[str, str]) -> None:
    # Validate the whole pending configuration before replacing saved choices.
    path = state_path(cfg)
    pending = json.loads(path.read_text(encoding="utf-8")) if path.is_file() else {}
    pending.update(choices)
    candidate = copy.deepcopy(cfg)
    apply(candidate, pending)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps(pending, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def snapshot(cfg: dict) -> dict[str, Any]:
    active = current(cfg)
    selected = dict(active)
    path = state_path(cfg)
    if path.is_file():
        try:
            selected.update(json.loads(path.read_text(encoding="utf-8")))
        except (OSError, ValueError, TypeError):
            pass
    available = {}
    for task, names in {"tts": ("omnivoice", "audiocpp"), "asr": ("audiocpp", "whisper")}.items():
        available[task] = []
        for name in names:
            candidate = copy.deepcopy(cfg)
            try:
                apply(candidate, {task: name})
                reason = ""
            except ValueError as exc:
                reason = str(exc)
            model = candidate["tts"].get("clone_model") or "built-in voices" if task == "tts" else candidate["asr"].get("default")
            available[task].append({"id": name, "model": model, "available": not reason, "reason": reason})
    return {"active": active, "selected": selected, "available": available, "restart_required": selected != active}
