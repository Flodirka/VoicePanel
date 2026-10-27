"""Configuration loading and defaults.

Paths inside the YAML file are resolved relative to the repository root (the
directory that contains the config file's parent tree, i.e. ``<repo>/config.yaml``
resolves relative paths against ``<repo>``). Environment expansion with
``${VAR}`` is supported.
"""
from __future__ import annotations

import os
import re
from pathlib import Path
from typing import Any

import yaml

_ENV_RE = re.compile(r"\$\{([^}^{]+)\}")

DEFAULTS: dict[str, Any] = {
    "service": {
        "host": "127.0.0.1",
        "port": 8080,
        # optional; when set, clients send an Authorization header. Default reads
        # the VOICE_SERVICE_AUTH_TOKEN environment variable (empty = no auth).
        "auth_token": os.environ.get("VOICE_SERVICE_AUTH_TOKEN", ""),
        "allow_origins": [],       # browser origins allowed to call the API; empty blocks all browser calls
        "max_upload_mb": 200,
        "request_timeout_s": 300,
    },
    "asr": {
        "default": "qwen3-asr",
    },
    "tts": {
        "default": "auto",         # auto = Kokoro for English; Cyrillic -> clone model + profile
        "clone_model": "qwen3-tts-clone",   # cloning model recorded for new voice profiles
    },
    "llm": {
        "enabled": True,
    },
    "sessions": {
        "vad_model": "runtime/models/silero_vad.onnx",
        "speculative": True,
        "speech_threshold": 0.5,
        "silence_frames": 24,          # 512-sample frames at 16 kHz (~0.77 s) to end an utterance
        "min_speech_frames": 6,
        "preroll_frames": 11,
        "max_utterance_s": 30.0,
        "speculate_every_frames": 63,
        "asr_model": None,             # None -> asr.default
    },
    "logging": {
        "transcripts": False,       # privacy: do not log transcript content by default
        "level": "info",
    },
    "backends": {
        # CPU-first defaults: the service must coexist with a GPU-bound agent LLM.
        # GPU profiles (cuda/vulkan binaries + backend + gpu_layers) are opt-in.
        "audiocpp": {
            "binary": "runtime/bin/audiocpp-cpu/audiocpp_server.exe",
            "backend": "cpu",
            "device": 0,
            "threads": 8,
            "host": "127.0.0.1",
            "port": 8101,
            "lazy_load": True,
            "idle_unload_ms": 90000,
            "max_loaded_models": 2,
            "start_timeout_s": 90,
            "idle_stop_s": 1800,       # stop the process when unused for this long
            "espeak_lib": "runtime/third_party/espeak-ng/eSpeak NG/libespeak-ng.dll",
            "espeak_data": "runtime/third_party/espeak-ng/eSpeak NG/espeak-ng-data",
            "voice_dir": "runtime/voice-library",   # zero-shot cloning profiles (see /v1/voices)
            "models": [
                {
                    "id": "qwen3-asr",
                    "family": "qwen3_asr",
                    "path": "runtime/models/qwen3-asr-0.6b-q8_0.gguf",
                    "task": "asr",
                    "mode": "offline",
                    "language": "auto",
                },
                {
                    "id": "kokoro",
                    "family": "kokoro_tts",
                    "path": "runtime/models/kokoro-82m-q8_0.gguf",
                    "task": "tts",
                    "mode": "offline",
                    "voice": "af_heart",
                },
            ],
        },
        # Zero-shot cloning runs in its own process so ASR and the built-in
        # voices can stay on the CPU. The GPU helper is started by the first
        # cloning request and stopped again after idle_stop_s (VRAM freed).
        "audiocpp_clone": {
            "enabled": True,
            "binary": "runtime/bin/audiocpp-cuda/audiocpp_server.exe",
            "backend": "cuda",
            "device": 0,
            "threads": 8,
            "host": "127.0.0.1",
            "port": 8104,
            "lazy_load": True,
            "idle_unload_ms": 60000,
            "max_loaded_models": 1,
            "start_timeout_s": 90,
            "idle_stop_s": 120,
            "voice_dir": "runtime/voice-library",
            "models": [
                {
                    "id": "qwen3-tts-clone",
                    "family": "qwen3_tts",
                    "path": "runtime/models/qwen3-tts-12hz-0.6b-base-q8_0.gguf",
                    "task": "tts",
                    "mode": "offline",
                    "session_options": {
                        "qwen3_tts.voice_prompt_cache_slots": "4",
                    },
                },
            ],
        },
        "whisper": {
            "enabled": True,
            "binary": "runtime/bin/whisper-cpu/Release/whisper-server.exe",
            "host": "127.0.0.1",
            "port": 8102,
            "model_path": "runtime/models/ggml-small.bin",
            "language": "auto",
            "threads": 8,
            "start_timeout_s": 60,
            "idle_stop_s": 1800,
            "extra_args": [],
        },
        "llama": {
            "binary": "runtime/bin/llama-vulkan/llama-server.exe",
            "variant_dirs": [          # first existing variant wins; enables cuda->vulkan->cpu fallback
                "runtime/bin/llama-cuda",
                "runtime/bin/llama-vulkan",
            ],
            "host": "127.0.0.1",
            "port": 8103,
            "model_path": "runtime/models/qwen3-4b-instruct-2507-q4_k_m.gguf",
            "model_alias": "qwen3-4b",
            "ctx_size": 8192,
            "gpu_layers": 0,
            "flash_attn": True,
            "threads": 8,
            "start_timeout_s": 180,
            "idle_stop_s": 900,
            "extra_args": [],
        },
    },
}


def _expand_env(value: Any) -> Any:
    if isinstance(value, str):
        return _ENV_RE.sub(lambda m: os.environ.get(m.group(1), ""), value)
    if isinstance(value, list):
        return [_expand_env(v) for v in value]
    if isinstance(value, dict):
        return {k: _expand_env(v) for k, v in value.items()}
    return value


def _deep_merge(base: dict[str, Any], override: dict[str, Any]) -> dict[str, Any]:
    out = dict(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(out.get(key), dict):
            out[key] = _deep_merge(out[key], value)
        else:
            out[key] = value
    return out


def _resolve_paths(node: Any, root: Path) -> Any:
    """Resolve path-like string values relative to the repo root.

    Values under keys ending in ``_path``/``_dir``/``_file``, plus ``binary`` and
    ``path`` keys, are treated as paths. Absolute paths are kept as-is.
    """
    if isinstance(node, dict):
        out = {}
        for key, value in node.items():
            if isinstance(value, str) and (
                key in ("binary", "path", "espeak_lib", "espeak_data", "vad_model")
                or key.endswith(("_path", "_dir", "_file"))
            ):
                p = Path(value)
                out[key] = str(p if p.is_absolute() else (root / p))
            else:
                out[key] = _resolve_paths(value, root)
        return out
    if isinstance(node, list):
        return [_resolve_paths(item, root) for item in node]
    return node


def load_config(path: str | Path | None = None) -> dict[str, Any]:
    """Load configuration; missing file -> defaults; provided file -> merged over defaults."""
    root = _find_root(path)
    cfg = DEFAULTS
    if path is not None and Path(path).is_file():
        with open(path, "r", encoding="utf-8") as fh:
            user = yaml.safe_load(fh) or {}
        cfg = _deep_merge(DEFAULTS, _expand_env(user))
    resolved = _resolve_paths(cfg, root)
    resolved["_root"] = str(root)
    return resolved


def _find_root(path: str | Path | None) -> Path:
    """The runtime root for relative paths: walk up from the config file (or cwd)
    to the directory containing ``runtime/`` or ``service/``."""
    start = Path(path).resolve().parent if path else Path.cwd()
    for candidate in [start, *start.parents]:
        if (candidate / "runtime").is_dir() or (candidate / "service").is_dir():
            return candidate
        if candidate.parent == candidate:
            break
    return start
