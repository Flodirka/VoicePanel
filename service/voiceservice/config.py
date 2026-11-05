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
        "default": "nemotron",     # fastest measured recogniser (0.41-0.46 RTF CPU)
    },
    "tts": {
        "default": "auto",         # auto = Kokoro (en) / Piper ru built-ins; clone profiles on explicit voice
        "clone_model": "omnivoice-q8",   # OmniVoice 0.6B Q8_0 (~945 MB, 600+ languages, zero-shot clone) via omnivoice.cpp. Alternative: qwen3-tts-clone (1.9 GB, ru/en only) via audio.cpp.
        "default_voices": {"en": "", "ru": ""},
    },
    "llm": {
        "enabled": True,
        # Correction-mode sentences with no dictionary fixes and no spell-checker
        # flags are returned unchanged without calling the model (see
        # api/text.py). Default on: most dictated sentences are already clean, and
        # skipping the model is what makes a small corrector fast enough.
        "skip_if_clean": True,
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
                # Fastest bilingual recogniser measured (0.41-0.46 RTF on CPU).
                # Punctuates in English only, so pair it with cleanup on demand.
                {
                    "id": "nemotron",
                    "family": "nemotron_asr",
                    "path": "runtime/models/nemotron-3.5-asr-streaming-0.6b-q8_0.gguf",
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
                    "language": "en",      # routing hint only; not sent to the server for tts
                },
                # Small fixed-speaker Russian voices (62 MB each, CPU, main
                # process): the defaults for Russian speech so a clone model is
                # not loaded just to read a sentence. Cloning stays available
                # for profiles via the clone engine on demand.
                {
                    "id": "piper-ru",
                    "family": "piper_tts",
                    "path": "runtime/models/piper-ru-ruslan-medium.gguf",
                    "task": "tts",
                    "mode": "offline",
                    "language": "ru",      # routing hint only
                    "supports_language": False,
                },
                {
                    "id": "piper-ru-female",
                    "family": "piper_tts",
                    "path": "runtime/models/piper-ru-irina-medium.gguf",
                    "task": "tts",
                    "mode": "offline",
                    "language": "ru",      # routing hint only
                    "supports_language": False,
                },
            ],
        },
        # Speaker separation runs in its own process so it can be placed on the
        # GPU while recognition stays on the CPU (or the reverse).
        "audiocpp_diar": {
            "enabled": True,
            "binary": "runtime/bin/audiocpp-cpu/audiocpp_server.exe",
            "backend": "cpu",
            "device": 0,
            "threads": 8,
            "host": "127.0.0.1",
            "port": 8105,
            "lazy_load": True,
            "idle_unload_ms": 120000,
            "max_loaded_models": 1,
            "start_timeout_s": 90,
            "models": [
                {
                    "id": "nemotron-diar",
                    "family": "nemotron_3_diar",
                    "path": "runtime/models/nemotron-3-diarization-q8_0.gguf",
                    "task": "diar",
                    "mode": "offline",
                },
            ],
        },
        # Zero-shot cloning runs in its own engine so ASR and the built-in
        # voices can stay on the CPU. The default engine is omnivoice.cpp
        # (section `omnivoice` below). `audiocpp_clone` is the audio.cpp-based
        # qwen3-tts-clone ALTERNATIVE: enable it, disable `omnivoice`, and set
        # tts.clone_model to qwen3-tts-clone to switch back.
        "audiocpp_clone": {
            "enabled": False,
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
        # The default cloning engine: k2-fsa omnivoice.cpp driving OmniVoice
        # 0.6B Q8_0 (~945 MB total, 600+ languages, zero-shot cloning; weights
        # CC-BY-NC 4.0 - download them yourself, see README). A stdin/stdout
        # CLI rather than an HTTP server, so the service supervises it through
        # backends/omnivoice.py. The `clone` compute group swaps `binary`/
        # `codec_binary` between the omnivoice-cpu and omnivoice-cuda builds.
        "omnivoice": {
            "enabled": True,
            "binary": "runtime/bin/omnivoice-cpu/omnivoice-tts.exe",
            "codec_binary": "runtime/bin/omnivoice-cpu/omnivoice-codec.exe",
            "backend": "cpu",
            "threads": 8,
            "steps": 32,               # MaskGIT decode steps; fewer is faster, lower quality
            "voice_dir": "runtime/voice-library",
            "idle_stop_s": 300,        # stop the child (frees ~1 GB RAM) when idle
            "start_timeout_s": 120,
            "request_timeout_s": 600,  # per request; CPU RTF is ~10-24 under load
            "max_line_chars": 300,     # texts are split into sentence lines first
            "models": [
                {
                    "id": "omnivoice-q8",
                    "family": "omnivoice",
                    "path": "runtime/models/omnivoice/omnivoice-base-Q8_0.gguf",
                    "codec_path": "runtime/models/omnivoice/omnivoice-tokenizer-Q8_0.gguf",
                    "task": "tts",
                    "mode": "offline",
                    "language": "auto",
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
            # Small GEC corrector (563 MB). The service always sends
            # enable_thinking=false, which this model needs (thinking mode
            # produces empty/wrong output — measured).
            "binary": "runtime/bin/llama-vulkan/llama-server.exe",
            "variant_dirs": [          # first existing variant wins; enables cuda->vulkan->cpu fallback
                "runtime/bin/llama-cuda",
                "runtime/bin/llama-vulkan",
            ],
            "host": "127.0.0.1",
            "port": 8103,
            "model_path": "runtime/models/Qwen3.5-0.8B-GEC-KAZ-RUS-ENG.Q4_0.gguf",
            "model_alias": "gec-0.8b",
            "ctx_size": 8192,
            "gpu_layers": 0,
            "flash_attn": True,
            "threads": 8,
            # llama-server keeps old prompt states in a host-RAM cache that
            # defaults to 8192 MiB. One dictation slot needs a few at most.
            "cache_ram_mib": 512,
            "cache_type_k": None,      # None = llama.cpp default (f16); q8_0 halves KV memory
            "cache_type_v": None,
            "start_timeout_s": 180,
            "idle_stop_s": 300,        # free RAM soon after a session ends
            "extra_args": [],
        },
        # Dedicated small translator: translation modes route here, correction
        # stays on `llama`. Base: PLaMo 3 Translate 0.8B Q4_K_M (573 MB), verified
        # in the full app flow (context/glossary via template kwargs, see
        # template_style below). Hy-MT2-1.8B Q4_K_M (1.1 GB) is the alternative -
        # swap model_path/model_alias and drop template_style to use it.
        "llama_translate": {
            "enabled": True,
            "binary": "runtime/bin/llama-vulkan/llama-server.exe",
            "variant_dirs": [
                "runtime/bin/llama-cuda",
                "runtime/bin/llama-vulkan",
            ],
            "host": "127.0.0.1",
            "port": 8106,
            "model_path": "runtime/models/plamo-3-translate/plamo-3-translate-0.8b-Q4_K_M.gguf",
            "model_alias": "plamo-3-translate-0.8b",
            "template_style": "plamo",
            "ctx_size": 8192,
            "gpu_layers": 0,
            "flash_attn": True,
            "threads": 8,
            "cache_ram_mib": 512,
            "start_timeout_s": 180,
            "idle_stop_s": 300,
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
