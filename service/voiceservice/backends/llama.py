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
            args += self.memory_args(cfg)
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
    def memory_args(cfg: dict[str, Any]) -> list[str]:
        """Host-memory controls for a llama-server the service starts itself.

        * ``cache_ram_mib``: the host prompt cache (``--cache-ram``). llama.cpp
          defaults to 8192 MiB and saves idle slot states into it, so a long
          session can grow the process by gigabytes. One dictation slot needs a
          few cached prompts at most. ``None`` keeps llama.cpp's default.
        * ``cache_type_k`` / ``cache_type_v``: KV cache storage (``f16`` is the
          llama.cpp default; ``q8_0`` halves KV memory, V needs flash attention).
        """
        args: list[str] = []
        cache_ram = cfg.get("cache_ram_mib")
        if cache_ram is not None:
            args += ["--cache-ram", str(int(cache_ram))]
        for key, flag in (("cache_type_k", "-ctk"), ("cache_type_v", "-ctv")):
            value = cfg.get(key)
            if value:
                args += [flag, str(value)]
        return args

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
            client = self.proc.http()
            url = f"{self.proc.base_url}/v1/chat/completions"
            stream = bool(payload.get("stream"))
            try:
                resp = await client.send(
                    client.build_request("POST", url, json=payload, timeout=timeout), stream=stream
                )
            except httpx.RemoteProtocolError:
                # stale pooled keep-alive connection; the server never saw it
                resp = await client.send(
                    client.build_request("POST", url, json=payload, timeout=timeout), stream=stream
                )
            try:
                yield resp
            finally:
                await resp.aclose()

    async def complete(self, messages: list[dict[str, Any]], **kw: Any) -> dict[str, Any]:
        """Non-streaming completion returning the response JSON."""
        # Backend-level template kwargs (config `chat_template_kwargs`) merge under
        # per-request ones, so a model-specific template (e.g. PLaMo's target_lang)
        # can be steered per request without touching its config.
        ctk = dict(self.cfg.get("chat_template_kwargs") or {})
        ctk.update(kw.pop("chat_template_kwargs", {}) or {})
        payload = {"messages": messages, "stream": False, **kw}
        if ctk:
            payload["chat_template_kwargs"] = ctk
        async with self.request(payload) as resp:
            resp.raise_for_status()
            return resp.json()

    async def stop(self) -> None:
        await self.proc.stop()
