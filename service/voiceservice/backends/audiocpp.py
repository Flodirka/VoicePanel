"""audio.cpp server backend: ASR (Qwen3-ASR) and TTS (Kokoro / Piper) models."""
from __future__ import annotations

import asyncio
import json
from pathlib import Path
from typing import Any

import httpx

from .process import Lease, ManagedProcess

# audio.cpp evicts an idle model before loading another one. A request that
# arrives during such a transition can fail once with a 5xx even though the
# retry succeeds, so every inference call retries a single time on 5xx.
_RETRY_STATUS = {500, 502, 503, 504}


async def _post_with_retry(
    client: httpx.AsyncClient, url: str, **kwargs: Any
) -> httpx.Response:
    try:
        resp = await client.post(url, **kwargs)
        resp.raise_for_status()
        return resp
    except httpx.HTTPStatusError as exc:
        if exc.response.status_code not in _RETRY_STATUS:
            raise
        await asyncio.sleep(1.0)
        resp = await client.post(url, **kwargs)
        resp.raise_for_status()
        return resp


class AudioCppBackend:
    """Generates the audio.cpp server config and proxies its endpoints."""

    def __init__(self, cfg: dict[str, Any], state_dir: Path, log_dir: Path | None = None) -> None:
        self.cfg = cfg
        self.models: dict[str, dict[str, Any]] = {m["id"]: m for m in cfg.get("models", [])}
        self.config_path = state_dir / f"audiocpp-{cfg['port']}.json"
        self._write_server_config()
        espeak_dir = str(Path(cfg["espeak_lib"]).parent) if cfg.get("espeak_lib") else None
        # Distinct name per instance: both audio.cpp processes must not share a
        # log file or a pid file (the clone helper runs on another port).
        proc_name = str(cfg.get("name") or f"audiocpp-{cfg['port']}")
        self.proc = ManagedProcess(
            name=proc_name,
            command=[cfg["binary"], "--config", str(self.config_path)],
            host=cfg.get("host", "127.0.0.1"),
            port=int(cfg["port"]),
            health_path="/health",
            health_check=lambda r: r.json().get("status") == "ok",
            start_timeout_s=float(cfg.get("start_timeout_s", 90)),
            idle_stop_s=float(cfg.get("idle_stop_s", 1800)),
            cwd=str(Path(cfg["binary"]).parent),
            log_dir=log_dir,
            # Kokoro and Piper both need eSpeak-ng for phonemes.
            prepend_path=[espeak_dir] if espeak_dir else [],
            extra_env=(
                {"ESPEAK_DATA_PATH": cfg["espeak_data"]} if cfg.get("espeak_data") else {}
            ),
        )

    def _write_server_config(self) -> None:
        self.config_path.parent.mkdir(parents=True, exist_ok=True)
        models = []
        for m in self.cfg.get("models", []):
            entry: dict[str, Any] = {
                "id": m["id"],
                "family": m["family"],
                "path": m["path"],
                "task": m["task"],
                "mode": m.get("mode", "offline"),
            }
            if m.get("session_options"):
                entry["session_options"] = m["session_options"]
            if m.get("default_request_options"):
                entry["default_request_options"] = m["default_request_options"]
            if m.get("voice") and m["family"] == "kokoro_tts":
                entry["default_voice_preset"] = {"voice_id": m["voice"]}
            if m["family"] == "piper_tts":
                # eSpeak-ng is required for phonemes; wire it explicitly so the
                # server works regardless of the caller's PATH.
                opts = entry.setdefault("session_options", {})
                if self.cfg.get("espeak_lib"):
                    opts["piper_tts.espeak_library_path"] = self.cfg["espeak_lib"]
                if self.cfg.get("espeak_data"):
                    opts["piper_tts.espeak_data_path"] = self.cfg["espeak_data"]
            if m.get("language") and m["task"] == "asr":
                entry.setdefault("default_request_options", {})["language"] = m["language"]
            models.append(entry)
        server = {
            "host": self.cfg.get("host", "127.0.0.1"),
            "port": int(self.cfg["port"]),
            "backend": self.cfg.get("backend", "cpu"),
            "device": int(self.cfg.get("device", 0)),
            "threads": int(self.cfg.get("threads", 4)),
            "lazy_load": bool(self.cfg.get("lazy_load", True)),
            "idle_unload_ms": int(self.cfg.get("idle_unload_ms", 90000)),
            "ui": False,
            "ui_management": True,
            "max_loaded_models": int(self.cfg.get("max_loaded_models", 2)),
            "models": models,
        }
        # Voice library for zero-shot cloning profiles (read per request; no restarts).
        if self.cfg.get("voice_dir"):
            server["voice_dir"] = self.cfg["voice_dir"]
        self.config_path.write_text(json.dumps(server, indent=2), encoding="utf-8")

    # ------------------------------------------------------------------ ASR
    async def transcribe(
        self,
        wav_bytes: bytes,
        *,
        model: str,
        language: str | None = None,
        prompt: str | None = None,
        timeout: float = 300.0,
    ) -> dict[str, Any]:
        """Returns {"text": str, ...}; audio must be 16 kHz mono PCM WAV."""
        async with self.proc.lease() as lease:  # noqa: F841 - lease keeps process alive
            async with httpx.AsyncClient(timeout=timeout, trust_env=False) as client:
                data = {"model": model, "response_format": "json"}
                if language:
                    data["language"] = language
                if prompt:
                    data["prompt"] = prompt
                resp = await _post_with_retry(
                    client,
                    f"{self.proc.base_url}/v1/audio/transcriptions",
                    files={"file": ("audio.wav", wav_bytes, "audio/wav")},
                    data=data,
                )
                return resp.json()
    async def transcribe_detailed(
        self, wav_bytes: bytes, *, model: str, language: str | None = None, timeout: float = 300.0
    ) -> dict[str, Any]:
        """Word-level detail variant when the model family supports it."""
        async with self.proc.lease():
            async with httpx.AsyncClient(timeout=timeout, trust_env=False) as client:
                data = {"model": model}
                if language:
                    data["language"] = language
                resp = await _post_with_retry(
                    client,
                    f"{self.proc.base_url}/v1/audio/transcriptions/details",
                    files={"file": ("audio.wav", wav_bytes, "audio/wav")},
                    data=data,
                )
                return resp.json()

    # ------------------------------------------------------------------ TTS
    async def speech(
        self,
        text: str,
        *,
        model: str,
        voice: str | None = None,
        speed: float | None = None,
        language: str | None = None,
        timeout: float = 300.0,
    ) -> bytes:
        async with self.proc.lease():
            async with httpx.AsyncClient(timeout=timeout, trust_env=False) as client:
                payload: dict[str, Any] = {"model": model, "input": text, "response_format": "wav"}
                if voice:
                    payload["voice"] = voice
                if speed is not None:
                    payload["speed"] = speed
                if language:
                    payload["language"] = language
                resp = await _post_with_retry(
                    client, f"{self.proc.base_url}/v1/audio/speech", json=payload
                )
                return resp.content

    async def loaded_models(self) -> list[dict[str, Any]]:
        if not self.proc.running:
            return []
        try:
            async with httpx.AsyncClient(timeout=5.0, trust_env=False) as client:
                resp = await client.get(f"{self.proc.base_url}/v1/models")
                resp.raise_for_status()
                return resp.json().get("data", [])
        except httpx.HTTPError:
            return []

    async def stop(self) -> None:
        await self.proc.stop()
