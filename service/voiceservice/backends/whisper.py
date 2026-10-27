"""whisper.cpp server backend (CPU) - fallback ASR and timestamped file transcription."""
from __future__ import annotations

from pathlib import Path
from typing import Any

import httpx

from .process import ManagedProcess


class WhisperBackend:
    def __init__(self, cfg: dict[str, Any], log_dir: Path | None = None) -> None:
        self.cfg = cfg
        args = [
            cfg["binary"],
            "--host", cfg.get("host", "127.0.0.1"),
            "--port", str(cfg["port"]),
            "-m", cfg["model_path"],
            "-l", cfg.get("language", "auto"),
            "-t", str(cfg.get("threads", 8)),
        ]
        args += list(cfg.get("extra_args", []))
        self.proc = ManagedProcess(
            name="whisper",
            command=args,
            host=cfg.get("host", "127.0.0.1"),
            port=int(cfg["port"]),
            health_path="/health",
            start_timeout_s=float(cfg.get("start_timeout_s", 60)),
            idle_stop_s=float(cfg.get("idle_stop_s", 1800)),
            cwd=str(Path(cfg["binary"]).parent),
            log_dir=log_dir,
        )

    async def infer(
        self,
        wav_bytes: bytes,
        *,
        language: str | None = None,
        prompt: str | None = None,
        temperature: float = 0.0,
        verbose: bool = False,
        timeout: float = 600.0,
    ) -> dict[str, Any]:
        """POST /inference (multipart). Audio must be 16 kHz mono PCM WAV.

        verbose=True asks whisper.cpp for its verbose_json (segments with
        timestamps + detected language + duration).
        """
        async with self.proc.lease() as lease:  # noqa: F841
            async with httpx.AsyncClient(timeout=timeout, trust_env=False) as client:
                data = {
                    "response_format": "verbose_json" if verbose else "json",
                    "temperature": str(temperature),
                }
                if language:
                    data["language"] = language
                if prompt:
                    data["prompt"] = prompt
                resp = await client.post(
                    f"{self.proc.base_url}/inference",
                    files={"file": ("audio.wav", wav_bytes, "audio/wav")},
                    data=data,
                )
                resp.raise_for_status()
                return resp.json()

    async def stop(self) -> None:
        await self.proc.stop()
