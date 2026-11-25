"""audio.cpp server backend: ASR (Qwen3-ASR) and TTS (Kokoro / Piper) models."""
from __future__ import annotations

import asyncio
import json
import os
import tempfile
from pathlib import Path
from typing import Any
from urllib.parse import urlencode

import httpx

from .process import Lease, ManagedProcess

# audio.cpp evicts an idle model before loading another one. A request that
# arrives during such a transition can fail once with a 5xx even though the
# retry succeeds, so every inference call retries a single time on 5xx.
_RETRY_STATUS = {500, 502, 503, 504}


async def _response_chunks(reader: asyncio.StreamReader, headers: dict[str, str]):
    """Read the native server's HTTP/1.1 response framing."""
    if "chunked" in headers.get("transfer-encoding", "").lower():
        while True:
            size = int((await reader.readline()).split(b";", 1)[0], 16)
            if size == 0:
                return
            remaining = size
            while remaining:
                data = await reader.readexactly(min(remaining, 65536))
                remaining -= len(data)
                yield data
            if await reader.readexactly(2) != b"\r\n":
                raise RuntimeError("invalid streaming response chunk")
    elif "content-length" in headers:
        remaining = int(headers["content-length"])
        while remaining:
            data = await reader.readexactly(min(remaining, 65536))
            remaining -= len(data)
            yield data
    else:
        while data := await reader.read(65536):
            yield data


async def _post_with_retry(
    client: httpx.AsyncClient, url: str, **kwargs: Any
) -> httpx.Response:
    try:
        resp = await client.post(url, **kwargs)
        resp.raise_for_status()
        return resp
    except httpx.RemoteProtocolError:
        # A pooled keep-alive connection that the server closed between
        # requests: the request never reached the model, so send it again.
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
    def supports_language(self, model: str) -> bool:
        """Whether this model declares a `language` request option.

        Parakeet-TDT rejects the field outright ("unknown Parakeet TDT request
        option: language"), which turns every request into a 500 until the
        entry is fixed. Model entries that cannot take a language set
        ``supports_language: false``.
        """
        return bool(self.models.get(model, {}).get("supports_language", True))

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
        async with self.proc.lease():  # the lease keeps the process alive
            data = {"model": model, "response_format": "json"}
            if language and self.supports_language(model):
                data["language"] = language
            if prompt:
                data["prompt"] = prompt
            resp = await _post_with_retry(
                self.proc.http(),
                f"{self.proc.base_url}/v1/audio/transcriptions",
                files={"file": ("audio.wav", wav_bytes, "audio/wav")},
                data=data,
                timeout=timeout,
            )
            return resp.json()

    async def transcribe_detailed(
        self,
        wav_bytes: bytes,
        *,
        model: str,
        language: str | None = None,
        prompt: str | None = None,
        timeout: float = 300.0,
    ) -> dict[str, Any]:
        """Word-level detail variant when the model family supports it.

        The prompt is carried here too: the detail variant used to take no
        prompt at all, so a verbose_json request silently lost its recognition
        context while the plain one kept it — the caller could not tell from
        the answer that the glossary had been dropped.
        """
        async with self.proc.lease():
            data = {"model": model}
            if language and self.supports_language(model):
                data["language"] = language
            if prompt:
                data["prompt"] = prompt
            resp = await _post_with_retry(
                self.proc.http(),
                f"{self.proc.base_url}/v1/audio/transcriptions/details",
                files={"file": ("audio.wav", wav_bytes, "audio/wav")},
                data=data,
                timeout=timeout,
            )
            return resp.json()

    async def diarize(self, wav_bytes: bytes, *, model: str, timeout: float = 300.0) -> list[dict]:
        """Speaker turns for one WAV.

        Task-`diar` models are not served by the transcription route (it insists
        on transcript text), so this uses the generic task route, which takes a
        server-local path. The file is written beside the service and removed
        immediately; nothing is kept.
        """
        async with self.proc.lease():
            handle, path = tempfile.mkstemp(suffix=".wav", dir=str(self.config_path.parent))
            try:
                with os.fdopen(handle, "wb") as fh:
                    fh.write(wav_bytes)
                payload = {"model": model, "audio": path}
                resp = await _post_with_retry(
                    self.proc.http(),
                    f"{self.proc.base_url}/v1/tasks/run",
                    json=payload,
                    timeout=timeout,
                )
                data = resp.json()
                return data.get("speaker_turns") or []
            finally:
                try:
                    os.unlink(path)
                except OSError:
                    pass

    async def transcribe_live(
        self,
        pcm_chunks,
        *,
        model: str,
        language: str | None = None,
        sample_rate: int = 16000,
        channels: int = 1,
        timeout: float = 900.0,
    ):
        """Stream raw PCM16 to the engine's live route and yield its SSE lines.

        The caller owns the ASR lock for the whole stream: the engine serves one
        model at a time, and a live request holds it until the body closes.
        """
        params: dict[str, Any] = {
            "model": model,
            "sample_rate": int(sample_rate),
            "channels": int(channels),
            "sample_format": "s16le",
        }
        if language and self.supports_language(model):
            params["language"] = language
        async with self.proc.lease():
            # HTTPX sends the complete body before reading a response. Live
            # sessions need both directions concurrently, including before EOF.
            try:
                async with asyncio.timeout(timeout):
                    reader, writer = await asyncio.open_connection(self.proc.host, self.proc.port)
                    path = "/v1/audio/transcriptions/live?" + urlencode(params)
                    writer.write((
                        f"POST {path} HTTP/1.1\r\nHost: {self.proc.host}:{self.proc.port}\r\n"
                        "Content-Type: application/octet-stream\r\nAccept: text/event-stream\r\n"
                        "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
                    ).encode("ascii"))

                    async def upload():
                        try:
                            async for chunk in pcm_chunks:
                                if chunk:
                                    writer.write(f"{len(chunk):x}\r\n".encode() + chunk + b"\r\n")
                                    await writer.drain()
                            writer.write(b"0\r\n\r\n")
                            await writer.drain()
                        except BaseException:
                            writer.close()
                            raise

                    sender = asyncio.create_task(upload())
                    try:
                        status_line = await reader.readline()
                        if not status_line.startswith(b"HTTP/1."):
                            raise RuntimeError("engine closed the streaming connection")
                        status = int(status_line.split(b" ", 2)[1])
                        headers = {}
                        while True:
                            line = await reader.readline()
                            if line == b"\r\n":
                                break
                            if not line:
                                raise RuntimeError("engine closed the streaming headers")
                            key, _, value = line.decode("latin1").partition(":")
                            headers[key.lower()] = value.strip()
                        pending = b""
                        async for chunk in _response_chunks(reader, headers):
                            pending += chunk
                            if status != 200:
                                raise RuntimeError(f"live transcription failed ({status}): {pending[:400].decode('utf-8', 'replace')}")
                            while b"\n" in pending:
                                line, pending = pending.split(b"\n", 1)
                                if line.strip():
                                    yield line.rstrip(b"\r").decode("utf-8", "replace")
                        if status != 200:
                            raise RuntimeError(f"live transcription failed ({status})")
                        if pending.strip():
                            yield pending.decode("utf-8", "replace")
                        await sender
                    finally:
                        sender.cancel()
                        await asyncio.gather(sender, return_exceptions=True)
                        writer.close()
                        try:
                            await writer.wait_closed()
                        except ConnectionError:
                            pass
            except TimeoutError as exc:
                raise RuntimeError("live transcription timed out") from exc
            except ConnectionError as exc:
                raise RuntimeError("live transcription failed: engine connection closed") from exc

    # ------------------------------------------------------------------ TTS
    async def speech(
        self,
        text: str,
        *,
        model: str,
        voice: str | None = None,
        speed: float | None = None,
        language: str | None = None,
        options: dict[str, Any] | None = None,
        timeout: float = 300.0,
    ) -> bytes:
        async with self.proc.lease():
            payload: dict[str, Any] = {"model": model, "input": text, "response_format": "wav"}
            if voice:
                payload["voice"] = voice
            if speed is not None:
                payload["speed"] = speed
            # Not every TTS family declares a `language` option; the ones that do
            # not reject the whole request instead of ignoring the field.
            if language and self.supports_language(model):
                payload["language"] = language
            if options:
                payload["options"] = options
            resp = await _post_with_retry(
                self.proc.http(), f"{self.proc.base_url}/v1/audio/speech", json=payload, timeout=timeout
            )
            return resp.content

    async def loaded_models(self) -> list[dict[str, Any]]:
        if not self.proc.running:
            return []
        try:
            resp = await self.proc.http().get(f"{self.proc.base_url}/v1/models", timeout=5.0)
            resp.raise_for_status()
            return resp.json().get("data", [])
        except httpx.HTTPError:
            return []

    async def stop(self) -> None:
        await self.proc.stop()
