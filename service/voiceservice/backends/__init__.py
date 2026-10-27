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
        self.whisper: WhisperBackend | None = None
        if bcfg.get("whisper", {}).get("enabled", True):
            self.whisper = WhisperBackend(bcfg["whisper"], log_dir)
        self.llama: LlamaBackend | None = None
        llama_cfg = bcfg.get("llama", {})
        if cfg.get("llm", {}).get("enabled", True) and (
            llama_cfg.get("model_path") or llama_cfg.get("external")
        ):
            self.llama = LlamaBackend(llama_cfg, log_dir)
        # Serialise inference calls per engine: audiocpp serves one model at a
        # time, whisper is single-slot, llama handles its own parallelism.
        self._asr_lock = asyncio.Lock()
        self._tts_lock = asyncio.Lock()
        self._llm_semaphore = asyncio.Semaphore(4)

    @property
    def llm(self) -> LlamaBackend | None:
        """Alias so API code can speak of 'the llm backend'."""
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
        if self.audiocpp_clone is not None:
            for m in self.audiocpp_clone.cfg.get("models", []):
                if m.get("task") == "tts" and m["id"] not in ids:
                    ids.append(m["id"])
        return ids

    def tts_backend(self, model_id: str) -> AudioCppBackend:
        """The process that serves this TTS model (clone helper when it owns it)."""
        if self.audiocpp_clone is not None and model_id in self.audiocpp_clone.models:
            return self.audiocpp_clone
        return self.audiocpp

    def tts_backend_name(self, model_id: str) -> str:
        return "audiocpp_clone" if self.tts_backend(model_id) is self.audiocpp_clone else "audiocpp"

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

    def resolve_tts(self, model: str | None, text: str = "") -> str:
        target = (model or "").strip()
        if target in ("", "default", "tts", "auto"):
            default = self.cfg["tts"].get("default", "auto")
            if default in ("", "auto"):
                # Voice ids are not a reliable language hint: piper builds can be
                # English-only. Only the clone model can speak Russian here, and
                # the caller must supply a reference profile for it.
                if has_cyrillic(text) and "qwen3-tts-clone" in self.tts_model_ids():
                    return "qwen3-tts-clone"
                builtin = [i for i in self.tts_model_ids() if i != "qwen3-tts-clone"]
                return builtin[0] if builtin else (self.tts_model_ids()[0] if self.tts_model_ids() else "")
            return default
        if target not in self.tts_model_ids():
            raise KeyError(f"unknown TTS model '{target}'; available: {', '.join(self.tts_model_ids())}")
        return target

    # ------------------------------------------------------------------ status
    async def status(self) -> dict[str, Any]:
        out: dict[str, Any] = {
            "audiocpp": {**self.audiocpp.proc.status(), "models": await self.audiocpp.loaded_models()},
            "audiocpp_clone": (
                {**self.audiocpp_clone.proc.status(), "models": await self.audiocpp_clone.loaded_models()}
                if self.audiocpp_clone is not None
                else None
            ),
            "llm": self.llama.proc.status() if self.llama else None,
            "whisper": self.whisper.proc.status() if self.whisper else None,
        }
        return out

    async def stop_all(self) -> None:
        for backend in (self.audiocpp, self.audiocpp_clone, self.whisper, self.llama):
            if backend is not None:
                try:
                    await backend.stop()
                except Exception:
                    pass

    # ------------------------------------------------------------ compute
    def backend_process(self, group: str) -> ManagedProcess | None:
        """The supervised process behind a compute group (see compute.GROUPS)."""
        if group == "llm":
            return self.llama.proc if self.llama else None
        if group == "clone":
            return self.audiocpp_clone.proc if self.audiocpp_clone else None
        if group == "main":
            return self.audiocpp.proc
        return None

    def busy_reason(self, group: str) -> str | None:
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
            self.audiocpp_clone = AudioCppBackend(bcfg["audiocpp_clone"], self.state_dir, self.log_dir)
        else:
            self.llama = LlamaBackend(bcfg["llama"], self.log_dir)
