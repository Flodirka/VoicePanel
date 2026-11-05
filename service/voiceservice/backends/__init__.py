"""Backend registry: owns backend instances, routes requests, reports status."""
from __future__ import annotations

import asyncio
import copy
import logging
from pathlib import Path
from typing import Any

from .. import compute
from ..config import load_config
from .audiocpp import AudioCppBackend
from .llama import LlamaBackend
from .omnivoice import OmnivoiceBackend
from .process import ManagedProcess
from .whisper import WhisperBackend

log = logging.getLogger("voiceservice.backends")


class ComputeBusy(RuntimeError):
    """A process is serving a request; a device change must wait for idle."""


def has_cyrillic(text: str) -> bool:
    return any("\u0400" <= ch <= "\u04FF" for ch in text or "")


class Registry:
    def __init__(self, cfg: dict[str, Any], state_dir: Path, log_dir: Path) -> None:
        self.cfg = cfg
        self.state_dir = state_dir
        self.log_dir = log_dir
        bcfg = cfg.get("backends", {})
        self.audiocpp = AudioCppBackend(bcfg["audiocpp"], state_dir, log_dir)
        # Cloning helper: a separate process, GPU by default, started on demand.
        # Kept apart so ASR and the built-in voices stay on the CPU profile.
        self.audiocpp_clone: AudioCppBackend | None = None
        clone_cfg = bcfg.get("audiocpp_clone") or {}
        if clone_cfg.get("enabled", False):
            binary = Path(str(clone_cfg.get("binary", "")))
            if binary.is_file():
                self.audiocpp_clone = AudioCppBackend(clone_cfg, state_dir, log_dir)
            else:
                log.warning(
                    "audiocpp_clone enabled but binary is missing (%s); "
                    "cloning falls back to the main backend",
                    binary,
                )
        self.audiocpp_diar: AudioCppBackend | None = None
        diar_cfg = bcfg.get("audiocpp_diar") or {}
        if diar_cfg.get("enabled", False):
            binary = Path(str(diar_cfg.get("binary", "")))
            if binary.is_file():
                self.audiocpp_diar = AudioCppBackend(diar_cfg, state_dir, log_dir)
            else:
                log.warning("audiocpp_diar enabled but binary is missing (%s)", binary)
        # Cloning engine (k2-fsa omnivoice.cpp): a piped CLI instead of an HTTP
        # server, supervised by OmnivoiceBackend itself. The default clone
        # model; audio.cpp's qwen3 helper above is the config-swap alternative.
        self.omnivoice: OmnivoiceBackend | None = None
        ov_cfg = bcfg.get("omnivoice") or {}
        if ov_cfg.get("enabled", False):
            binary = Path(str(ov_cfg.get("binary", "")))
            if binary.is_file():
                self.omnivoice = OmnivoiceBackend(ov_cfg, log_dir)
            else:
                log.warning(
                    "omnivoice enabled but binary is missing (%s); "
                    "cloning falls back to the audio.cpp helper if configured",
                    binary,
                )
        self.whisper: WhisperBackend | None = None
        if bcfg.get("whisper", {}).get("enabled", True):
            self.whisper = WhisperBackend(bcfg["whisper"], log_dir)
        self.llama: LlamaBackend | None = None
        llama_cfg = bcfg.get("llama", {})
        if cfg.get("llm", {}).get("enabled", True) and (
            llama_cfg.get("model_path") or llama_cfg.get("external")
        ):
            self.llama = LlamaBackend(llama_cfg, log_dir)
        # Optional dedicated translation model (e.g. Hy-MT2-1.8B): translation
        # modes route to it when configured, correction stays on `llama`.
        # Its device is fixed by its own config; compute switching does not
        # cover it (it is not one of compute.GROUPS).
        self.llm_translate: LlamaBackend | None = None
        tr_cfg = bcfg.get("llama_translate") or {}
        if tr_cfg.get("enabled", True) and (tr_cfg.get("model_path") or tr_cfg.get("external")):
            self.llm_translate = LlamaBackend(tr_cfg, log_dir)
        # Serialise inference calls per engine: audiocpp serves one model at a
        # time, whisper is single-slot, llama handles its own parallelism.
        self._asr_lock = asyncio.Lock()
        self._tts_lock = asyncio.Lock()
        self._llm_semaphore = asyncio.Semaphore(4)
        # Session warm-up (api/warmup.py): kinds in progress and their tasks.
        self.warming: set[str] = set()
        self.background_tasks: set[asyncio.Task] = set()

    @property
    def llm(self) -> LlamaBackend | None:
        """Alias so API code can speak of 'the llm backend'."""
        return self.llama

    def llm_for(self, mode: str) -> LlamaBackend | None:
        """The backend a text mode runs on.

        Translation modes use the dedicated `llama_translate` backend when one
        is configured (a small specialist such as Hy-MT2-1.8B, ~3x faster than
        a general 4B on decode); everything else uses the main `llama`.
        """
        if mode in ("translate_ru_en", "translate_en_ru") and self.llm_translate is not None:
            return self.llm_translate
        return self.llama

    # ----------------------------------------------------------------- routing
    def asr_model_ids(self) -> list[str]:
        ids = []
        for m in self.cfg["backends"]["audiocpp"].get("models", []):
            if m.get("task") == "asr":
                ids.append(m["id"])
        if self.whisper:
            ids.append("whisper")
        return ids

    def tts_model_ids(self) -> list[str]:
        ids = [m["id"] for m in self.cfg["backends"]["audiocpp"].get("models", []) if m.get("task") == "tts"]
        if self.omnivoice is not None:
            for mid in self.omnivoice.models:
                if mid not in ids:
                    ids.append(mid)
        if self.audiocpp_clone is not None:
            for m in self.audiocpp_clone.cfg.get("models", []):
                if m.get("task") == "tts" and m["id"] not in ids:
                    ids.append(m["id"])
        return ids

    def tts_backend(self, model_id: str) -> "AudioCppBackend | OmnivoiceBackend":
        """The process that serves this TTS model (cloning engine first)."""
        if self.omnivoice is not None and model_id in self.omnivoice.models:
            return self.omnivoice
        if self.audiocpp_clone is not None and model_id in self.audiocpp_clone.models:
            return self.audiocpp_clone
        return self.audiocpp

    def tts_backend_name(self, model_id: str) -> str:
        backend = self.tts_backend(model_id)
        if backend is self.omnivoice:
            return "omnivoice"
        return "audiocpp_clone" if backend is self.audiocpp_clone else "audiocpp"

    def diar_model_ids(self) -> list[str]:
        """Diarisation models, from their own process or the main one."""
        ids: list[str] = []
        for section in ("audiocpp_diar", "audiocpp"):
            for m in self.cfg["backends"].get(section, {}).get("models", []):
                if m.get("task") == "diar" and m["id"] not in ids:
                    ids.append(m["id"])
        return ids

    def diar_backend(self, model_id: str) -> AudioCppBackend:
        """The process that serves this diarisation model."""
        if self.audiocpp_diar is not None and model_id in self.audiocpp_diar.models:
            return self.audiocpp_diar
        return self.audiocpp

    def resolve_diar(self, model: str | None) -> str:
        """Map a requested id to a configured task-`diar` model."""
        available = self.diar_model_ids()
        if not available:
            raise KeyError("no diarisation model is configured")
        target = (model or available[0]).strip()
        if target not in available:
            raise KeyError(f"unknown diarisation model '{target}'; available: {', '.join(available)}")
        return target

    def resolve_asr(self, model: str | None) -> tuple[str, str]:
        """Map a requested model id to (backend, model_id). Raises KeyError when unknown."""
        target = (model or self.cfg["asr"]["default"]).strip()
        if target == "default" or target == "asr":
            target = self.cfg["asr"]["default"]
        if target == "whisper" or target.startswith("whisper-"):
            if self.whisper is None:
                raise KeyError("whisper backend is disabled")
            return "whisper", "whisper"
        audiocpp_models = self.audiocpp.models
        if target in audiocpp_models and audiocpp_models[target].get("task") == "asr":
            return "audiocpp", target
        raise KeyError(f"unknown ASR model '{target}'; available: {', '.join(self.asr_model_ids())}")

    def _tts_model(self, model_id: str) -> dict[str, Any] | None:
        """The config entry for a TTS model, from any TTS-serving process."""
        entry = self.audiocpp.models.get(model_id)
        if entry is None and self.omnivoice is not None:
            entry = self.omnivoice.models.get(model_id)
        if entry is None and self.audiocpp_clone is not None:
            entry = self.audiocpp_clone.models.get(model_id)
        return entry

    def resolve_tts(self, model: str | None, text: str = "") -> str:
        target = (model or "").strip()
        if target in ("", "default", "tts", "auto"):
            default = self.cfg["tts"].get("default", "auto")
            if default in ("", "auto"):
                # Auto routing is by the text's script and the model entries'
                # `language` hint. A small per-language built-in (Kokoro en,
                # Piper-ru) wins over the clone model: cloning is for explicit
                # voice profiles, not everyday speech.
                ids = self.tts_model_ids()
                lang = "ru" if has_cyrillic(text) else "en"
                clone_id = self.cfg["tts"].get("clone_model", "omnivoice-q8")
                builtin = [
                    i for i in ids
                    if i != clone_id and str((self._tts_model(i) or {}).get("language", "")).lower() == lang
                ]
                if builtin:
                    return builtin[0]
                if lang == "ru" and clone_id in ids:
                    return clone_id
                rest = [i for i in ids if i != clone_id]
                return rest[0] if rest else (ids[0] if ids else "")
            return default
        if target not in self.tts_model_ids():
            raise KeyError(f"unknown TTS model '{target}'; available: {', '.join(self.tts_model_ids())}")
        return target

    # ------------------------------------------------------------------ status
    async def status(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "audiocpp": {**self.audiocpp.proc.status(), "models": await self.audiocpp.loaded_models()},
            "omnivoice": self.omnivoice.status() if self.omnivoice is not None else None,
            "audiocpp_clone": (
                {**self.audiocpp_clone.proc.status(), "models": await self.audiocpp_clone.loaded_models()}
                if self.audiocpp_clone is not None
                else None
            ),
            "audiocpp_diar": (
                {**self.audiocpp_diar.proc.status(), "models": await self.audiocpp_diar.loaded_models()}
                if self.audiocpp_diar is not None else None
            ),
            "llm": self.llama.proc.status() if self.llama else None,
            "whisper": self.whisper.proc.status() if self.whisper else None,
        }
        return out

    async def stop_all(self) -> None:
        for task in list(self.background_tasks):
            task.cancel()     # warm-ups must not start an engine during shutdown
        if self.background_tasks:
            await asyncio.gather(*self.background_tasks, return_exceptions=True)
        for backend in (self.audiocpp, self.audiocpp_diar, self.omnivoice, self.audiocpp_clone,
                        self.whisper, self.llama, self.llm_translate):
            if backend is not None:
                try:
                    await backend.stop()
                except Exception:
                    pass

    # ------------------------------------------------------------ compute
    def backend_process(self, group: str) -> ManagedProcess | None:
        """The supervised HTTP process behind a compute group (see compute.GROUPS).

        The omnivoice group is a piped CLI without a ManagedProcess; its busy
        state is checked directly in busy_reason.
        """
        if group == "llm":
            return self.llama.proc if self.llama else None
        if group == "main":
            return self.audiocpp.proc
        if group == "diar":
            return self.audiocpp_diar.proc if self.audiocpp_diar else None
        return None

    def busy_reason(self, group: str) -> str | None:
        if group == "clone" and self.omnivoice is not None:
            if self.omnivoice.status()["leases"] > 0:
                return "clone: a request is in progress"
            return None
        proc = self.backend_process(group)
        if proc is None:
            return None
        status = proc.status()
        if status["leases"] > 0:
            return f"{group}: a request is in progress"
        if status["adopted"]:
            return f"{group}: the process was started outside the service"
        return None

    async def apply_compute(self, devices: dict[str, str]) -> dict[str, str]:
        """Switch devices. Refuses while a changed process serves a request.

        The choice is validated first, then the idle process is stopped, the
        config is updated, the choice is persisted, and the backend is rebuilt
        so the next request starts on the new device.
        """
        unknown = set(devices) - set(compute.GROUPS)
        if unknown:
            raise ValueError(f"unknown group(s): {', '.join(sorted(unknown))}")
        current = compute.current_devices(self.cfg)
        changes = {group: devices[group] for group in compute.GROUPS
                   if group in devices and devices[group] != current[group]}
        if not changes:
            return current
        compute.apply_to_config(copy.deepcopy(self.cfg), changes)   # validate before any stop
        reasons = [reason for group in changes if (reason := self.busy_reason(group))]
        if reasons:
            raise ComputeBusy("; ".join(reasons))
        for group in changes:
            if group == "clone" and self.omnivoice is not None:
                await self.omnivoice.stop()
            else:
                proc = self.backend_process(group)
                if proc is not None:
                    await proc.stop()
        compute.apply_to_config(self.cfg, changes)
        compute.save_overrides(self.cfg["_root"], changes)
        for group in changes:
            self._rebuild(group)
        return compute.current_devices(self.cfg)

    def _rebuild(self, group: str) -> None:
        bcfg = self.cfg["backends"]
        if group == "main":
            self.audiocpp = AudioCppBackend(bcfg["audiocpp"], self.state_dir, self.log_dir)
        elif group == "clone":
            self.omnivoice = OmnivoiceBackend(bcfg["omnivoice"], self.log_dir)
        elif group == "diar":
            self.audiocpp_diar = AudioCppBackend(bcfg["audiocpp_diar"], self.state_dir, self.log_dir)
        else:
            self.llama = LlamaBackend(bcfg["llama"], self.log_dir)
