"""llama.cpp llama-server backend for text translation and correction."""
from __future__ import annotations

from contextlib import asynccontextmanager
from pathlib import Path
from typing import Any, AsyncIterator

import httpx

from .process import ManagedProcess


class LlamaBackend:
    def __init__(self, cfg: dict[str, Any], log_dir: Path | None = None) -> None:
        self.cfg = cfg
        # external: an already running llama.cpp server (the agent's own model,
        # for instance). The service adopts it and never starts or stops it.
        self.external = bool(cfg.get("external", False))
        binary = str(cfg.get("binary", "llama-server"))
        if self.external:
            args = [binary]
        else:
            binary = self._resolve_binary(cfg)
            args = [
                binary,
                "-m", cfg["model_path"],
                "--host", cfg.get("host", "127.0.0.1"),
                "--port", str(cfg["port"]),
                "-c", str(cfg.get("ctx_size", 8192)),
                "-t", str(cfg.get("threads", 8)),
                "--jinja",
                "--parallel", "1",
            ]
            if cfg.get("gpu_layers", 0) > 0:
                args += ["-ngl", str(cfg["gpu_layers"])]
            if cfg.get("flash_attn", True):
                args += ["-fa", "auto"]
            args += list(cfg.get("extra_args", []))
        self.proc = ManagedProcess(
            name="llama",
            command=args,
            host=cfg.get("host", "127.0.0.1"),
            port=int(cfg["port"]),
            health_path="/health",
            start_timeout_s=float(cfg.get("start_timeout_s", 180)),
            idle_stop_s=float(cfg.get("idle_stop_s", 900)),
            cwd=str(Path(binary).parent),
            log_dir=log_dir,
            spawn=not self.external,
        )

    @staticmethod
    def _resolve_binary(cfg: dict[str, Any]) -> str:
        """Prefer the configured binary; fall back through variant dirs when missing."""
        binary = cfg["binary"]
        if Path(binary).is_file():
            return binary
        for variant in cfg.get("variant_dirs", []):
            exe = Path(variant) / "llama-server.exe"
            if exe.is_file():
                return str(exe)
        return binary

    @property
    def model_alias(self) -> str:
        return self.cfg.get("model_alias", "local-llm")

    @asynccontextmanager
    async def request(self, payload: dict[str, Any], *, timeout: float = 300.0) -> AsyncIterator[httpx.Response]:
        """Yield an httpx response (streamed or not) while holding a process lease."""
        async with self.proc.lease():
            client = httpx.AsyncClient(timeout=timeout, trust_env=False)
            try:
                req = client.build_request(
                    "POST", f"{self.proc.base_url}/v1/chat/completions", json=payload
                )
                resp = await client.send(req, stream=bool(payload.get("stream")))
                try:
                    yield resp
                finally:
                    await resp.aclose()
            finally:
                await client.aclose()

    async def complete(self, messages: list[dict[str, Any]], **kw: Any) -> dict[str, Any]:
        """Non-streaming completion returning the response JSON."""
        payload = {"messages": messages, "stream": False, **kw}
        async with self.request(payload) as resp:
            resp.raise_for_status()
            return resp.json()

    async def stop(self) -> None:
        await self.proc.stop()
