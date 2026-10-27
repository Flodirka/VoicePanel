"""CPU/GPU placement for the inference process groups the panel exposes.

The service runs inference in three supervised processes:

* ``llm``   - llama.cpp: translation and correction
* ``clone`` - audio.cpp helper: cloned-voice TTS only
* ``main``  - audio.cpp: speech recognition and the built-in voices, one process

Recognition and the built-in voices cannot be split: audio.cpp takes one device
per server process, so both always share one choice.

A saved choice lives in ``runtime/state/compute.json`` and is laid over
``config.yaml`` at startup, so the commented configuration file stays intact.
At runtime ``Registry.apply_compute`` switches devices only while the affected
process is idle.
"""
from __future__ import annotations

import json
import logging
from pathlib import Path
from typing import Any

log = logging.getLogger("voiceservice.compute")

DEVICES = ("cpu", "gpu")

GROUPS: dict[str, dict[str, str]] = {
    "llm": {"label": "Translation and correction", "process": "llama"},
    "clone": {"label": "Cloned voice TTS", "process": "audiocpp_clone"},
    "main": {"label": "Recognition and built-in voices (one process)", "process": "audiocpp"},
}

# audio.cpp: the CUDA build also runs on the CPU, so the device is `backend`.
_AUDIOCPP_VARIANTS = {"cpu": ("audiocpp-cpu", "cpu"), "gpu": ("audiocpp-cuda", "cuda")}
_LLAMA_GPU_VARIANT = "llama-cuda"
_LLAMA_CPU_VARIANT = "llama-vulkan"   # a GPU build with gpu_layers 0 stays on the CPU
_LLAMA_GPU_LAYERS = 99


def state_path(root: str | Path) -> Path:
    return Path(root) / "runtime" / "state" / "compute.json"


def load_overrides(root: str | Path) -> dict[str, str]:
    path = state_path(root)
    if not path.is_file():
        return {}
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        log.warning("ignoring unreadable compute settings: %s", path)
        return {}
    if not isinstance(data, dict):
        return {}
    return {
        str(group): str(device).lower()
        for group, device in data.items()
        if group in GROUPS and str(device).lower() in DEVICES
    }


def save_overrides(root: str | Path, choices: dict[str, str]) -> Path:
    path = state_path(root)
    path.parent.mkdir(parents=True, exist_ok=True)
    merged = {**load_overrides(root), **choices}
    path.write_text(json.dumps(merged, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return path


def current_devices(cfg: dict[str, Any]) -> dict[str, str]:
    """The device each group runs on, read back from the effective config."""
    backends = cfg.get("backends", {})
    llama = backends.get("llama") or {}
    return {
        "llm": "gpu" if int(llama.get("gpu_layers", 0) or 0) > 0 else "cpu",
        "clone": _audiocpp_device(backends.get("audiocpp_clone")),
        "main": _audiocpp_device(backends.get("audiocpp")),
    }


def available_devices(cfg: dict[str, Any]) -> dict[str, list[str]]:
    """Devices whose build exists on disk; CPU is always offered."""
    backends = cfg.get("backends", {})
    available: dict[str, list[str]] = {}
    for group in GROUPS:
        devices = ["cpu"]
        gpu_binary = _gpu_binary(group, backends)
        if gpu_binary and Path(gpu_binary).is_file():
            devices.append("gpu")
        available[group] = devices
    return available


def apply_to_config(cfg: dict[str, Any], choices: dict[str, str]) -> None:
    """Point the backend sections at the chosen builds. Raises ValueError."""
    backends = cfg.setdefault("backends", {})
    for group, device in choices.items():
        if group not in GROUPS:
            raise ValueError(f"unknown group '{group}'")
        if device not in DEVICES:
            raise ValueError(f"unknown device '{device}' for {group}")
        if group == "llm":
            _apply_llm(backends.setdefault("llama", {}), device)
        else:
            key = "audiocpp_clone" if group == "clone" else "audiocpp"
            section = backends.get(key)
            if not section:
                raise ValueError(f"the {group} backend is not configured")
            if key == "audiocpp_clone" and not section.get("enabled", False):
                raise ValueError("the cloned-voice process is disabled in the configuration")
            _apply_audiocpp(section, device)


def apply_overrides(cfg: dict[str, Any]) -> dict[str, str]:
    """Lay the saved choices over the loaded config; bad choices are ignored."""
    overrides = load_overrides(cfg.get("_root", "."))
    if not overrides:
        return {}
    try:
        apply_to_config(cfg, overrides)
    except ValueError as exc:
        log.warning("ignoring saved compute choice: %s", exc)
        return {}
    return overrides


# --------------------------------------------------------------------- helpers
def _audiocpp_device(section: dict[str, Any] | None) -> str:
    if not section:
        return "cpu"
    return "cpu" if str(section.get("backend", "cpu")).lower() == "cpu" else "gpu"


def _sibling_binary(binary: str, variant: str) -> Path:
    """The same executable inside another build directory beside the current one."""
    path = Path(binary)
    return path.parent.parent / variant / path.name


def _gpu_binary(group: str, backends: dict[str, Any]) -> str | None:
    if group == "llm":
        binary = (backends.get("llama") or {}).get("binary")
        return str(_sibling_binary(str(binary), _LLAMA_GPU_VARIANT)) if binary else None
    key = "audiocpp_clone" if group == "clone" else "audiocpp"
    binary = (backends.get(key) or {}).get("binary")
    if not binary:
        return None
    return str(_sibling_binary(str(binary), _AUDIOCPP_VARIANTS["gpu"][0]))


def _apply_audiocpp(section: dict[str, Any], device: str) -> None:
    variant, backend = _AUDIOCPP_VARIANTS[device]
    binary = _sibling_binary(str(section.get("binary", "")), variant)
    if not binary.is_file():
        raise ValueError(f"the {device} build is missing ({binary})")
    section["binary"] = str(binary)
    section["backend"] = backend


def _apply_llm(llama: dict[str, Any], device: str) -> None:
    if device == "gpu":
        gpu_binary = _gpu_binary("llm", {"llama": llama})
        if not gpu_binary or not Path(gpu_binary).is_file():
            raise ValueError("the GPU build of the language model is missing")
        llama["binary"] = gpu_binary
        llama["gpu_layers"] = max(int(llama.get("gpu_layers", 0) or 0), _LLAMA_GPU_LAYERS)
        return
    cpu_binary = _sibling_binary(str(llama.get("binary", "")), _LLAMA_CPU_VARIANT)
    if cpu_binary.is_file():
        llama["binary"] = str(cpu_binary)
    llama["gpu_layers"] = 0
