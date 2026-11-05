"""omnivoice.cpp backend: zero-shot voice cloning TTS via a piped CLI.

The engine (k2-fsa omnivoice.cpp, built into runtime/bin/omnivoice-<cpu|cuda>/)
is a stdin/stdout tool, not an HTTP server, so unlike the audio.cpp backends it
is supervised here directly:

* one warm child process with ``--stream-by-line -o -``; every input line
  synthesises to one utterance, written to stdout as a 44-byte RIFF header
  (live marker, no sizes) followed by PCM and flushed immediately;
* a request writes one line and reads utterances until the stream stalls, so
  no sentinel protocol is needed;
* the child is bound to (model, voice, language): changing any of them
  restarts it with the matching arguments. References are pre-encoded once
  per profile with ``omnivoice-codec`` (a tiny .rvq) and cached beside the
  profile WAV, so a voice switch costs a restart, not a re-encode;
* the child is stopped after ``idle_stop_s`` idle seconds (frees ~1 GB RAM;
  the CUDA build also frees VRAM) and restarted on the next request.

Long texts are split into sentence lines before sending (the engine's own
chunking is bypassed) and the PCM of the resulting utterances is concatenated
into a single 24 kHz mono WAV, which is the format the codec always emits.
"""
from __future__ import annotations

import asyncio
import os
import queue
import re
import struct
import subprocess
import sys
import threading
import time
from pathlib import Path
from typing import Any

CREATE_NO_WINDOW = 0x08000000 if sys.platform == "win32" else 0
SAMPLE_RATE = 24000
# Reply completion is a quiet stdout pipe. Two thresholds: waiting for the
# first PCM byte takes as long as model load + LLM forward (tens of seconds on
# CPU), but once PCM flows the codec decoder writes steadily, so a short gap
# ends the reply without a noticeable latency penalty.
STALL_S = 20.0
QUIET_STALL_S = 2.0
_SENTENCE_END = re.compile(r"(?<=[.!?…。])\s+")
_AUTO_LANGS = ("", "auto", "none", "null")


class OmnivoiceError(RuntimeError):
    pass


class OmnivoiceBackend:
    def __init__(self, cfg: dict[str, Any], log_dir: Path | None = None) -> None:
        self.cfg = cfg
        self.log_dir = log_dir
        self.models: dict[str, dict[str, Any]] = {
            m["id"]: m for m in cfg.get("models", []) if m.get("task") == "tts"
        }
        self.threads = int(cfg.get("threads", 8))
        self.idle_stop_s = float(cfg.get("idle_stop_s", 300))
        self.start_timeout_s = float(cfg.get("start_timeout_s", 120))
        self.request_timeout_s = float(cfg.get("request_timeout_s", 600))
        self.max_line_chars = int(cfg.get("max_line_chars", 300))
        self.steps = int(cfg.get("steps", 32))
        self.stall_s = float(cfg.get("stall_s", STALL_S))
        self.quiet_stall_s = float(cfg.get("quiet_stall_s", QUIET_STALL_S))
        self.voice_dir = Path(cfg.get("voice_dir", "runtime/voice-library"))
        self._proc: subprocess.Popen | None = None
        self._out: "queue.Queue[bytes]" = queue.Queue()
        self._reader: threading.Thread | None = None
        self._key: tuple[str, str, str] | None = None   # (model_id, voice_id, lang)
        self._lock = asyncio.Lock()                     # one synthesis at a time
        self._leases = 0
        self._last_used = time.monotonic()
        self._started_at: float | None = None
        self._restarts = 0
        self._idle_task: asyncio.Task | None = None
        self._log_file = None

    # ------------------------------------------------------------------ status
    def status(self) -> dict[str, Any]:
        running = self.running
        return {
            "name": "omnivoice",
            "running": running,
            "adopted": False,
            "pid": self._proc.pid if running and self._proc else None,
            "leases": self._leases,
            "restarts": self._restarts,
            "uptime_s": (
                round(time.monotonic() - self._started_at, 1)
                if running and self._started_at else None
            ),
            "idle_s": (
                round(time.monotonic() - self._last_used, 1)
                if running and self._leases == 0 else 0.0
            ),
        }

    @property
    def running(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    # ------------------------------------------------------------------- synth
    async def speech(
        self,
        text: str,
        *,
        model: str,
        voice: str | None = None,
        speed: float | None = None,
        language: str | None = None,
        options: dict[str, Any] | None = None,
        timeout: float | None = None,
    ) -> bytes:
        """Synthesize one text to a 24 kHz mono WAV. Clones when `voice` is a profile id.

        ``options`` is accepted for interface parity with the other TTS
        backends; the CLI has no per-request knobs beyond the process args
        (``max_tokens`` and friends do not apply to this engine).
        """
        if speed is not None:
            raise OmnivoiceError("the omnivoice engine has no speed control")
        if model not in self.models:
            raise OmnivoiceError(f"unknown omnivoice model '{model}'")
        lang = "" if str(language or "").lower() in _AUTO_LANGS else str(language)
        lines = self._split_lines(text)
        timeout = timeout or self.request_timeout_s
        async with self._lock:
            self._leases += 1
            self._last_used = time.monotonic()
            try:
                await asyncio.to_thread(self._ensure_process, model, voice or "", lang)
                deadline = time.monotonic() + timeout
                chunks: list[bytes] = []
                for line in lines:
                    remaining = deadline - time.monotonic()
                    if remaining <= 0:
                        raise OmnivoiceError("synthesis timed out before finishing")
                    chunks.append(await asyncio.to_thread(self._synthesize_line, line, remaining))
                return join_pcm(chunks)
            finally:
                self._leases -= 1
                self._last_used = time.monotonic()
                self._ensure_idle_watcher()

    def _split_lines(self, text: str) -> list[str]:
        """Sentence lines the engine turns into one utterance each."""
        text = " ".join((text or "").split())
        if not text:
            raise OmnivoiceError("input text is empty")
        if not _SENTENCE_END.search(text):
            text = text.rstrip(".!?…。") + "."
        parts = [p.strip() for p in _SENTENCE_END.split(text) if p.strip()]
        lines: list[str] = []
        for part in parts:
            while len(part) > self.max_line_chars:
                cut = part.rfind(" ", 0, self.max_line_chars)
                cut = cut if cut > self.max_line_chars // 2 else self.max_line_chars
                lines.append(part[:cut].rstrip() + ".")
                part = part[cut:].lstrip()
            lines.append(part)
        return lines

    # ------------------------------------------------------------------ process
    def _command(self, entry: dict[str, Any], voice: str, lang: str) -> list[str]:
        args = [
            str(self.cfg.get("binary", "")),
            "--model", str(entry["path"]),
            "--codec", str(entry["codec_path"]),
            "--stream-by-line", "-o", "-",
            "--steps", str(self.steps),
            # One utterance per line: the service chunks sentences itself.
            "--chunk-duration", "999",
            "--chunk-threshold", "999",
        ]
        if lang:
            args += ["--lang", lang]
        if voice:
            args += self._reference_args(voice)
        return args

    def _reference_args(self, voice: str) -> list[str]:
        """--ref-rvq/--ref-text for a profile, encoding the reference once."""
        wav = self.voice_dir / f"{voice}.wav"
        if not wav.is_file():
            raise OmnivoiceError(
                f"voice profile '{voice}' has no reference WAV in {self.voice_dir}"
            )
        rvq = self.voice_dir / f"{voice}.rvq"
        if not rvq.is_file():
            self._encode_reference(voice, wav, rvq)
        text_path = self.voice_dir / f"{voice}.ref.txt"
        text_path.write_text(self._profile_transcript(voice), encoding="utf-8")
        return ["--ref-rvq", str(rvq), "--ref-text", str(text_path)]

    def _profile_transcript(self, voice: str) -> str:
        """Transcript from the voice library's prompt_text ("<id>|<text>" lines)."""
        prompt = self.voice_dir / "prompt_text"
        if prompt.is_file():
            for line in prompt.read_text(encoding="utf-8").splitlines():
                vid, _, text = line.partition("|")
                if vid.strip() == voice and text.strip():
                    return text.strip()
        raise OmnivoiceError(
            f"voice profile '{voice}' has no transcript in prompt_text; "
            "cloning quality needs one"
        )

    def _encode_reference(self, voice: str, wav: Path, rvq: Path) -> None:
        """Pre-encode a reference WAV with omnivoice-codec (RMS gain, silence trim).

        The codec names its output after the input, so the copy is called
        ``<id>-enc.wav`` and the codes land in ``<id>-enc.rvq``.
        """
        codec = str(self.cfg.get("codec_binary", ""))
        if not codec or not Path(codec).is_file():
            raise OmnivoiceError(f"omnivoice-codec binary is missing: {codec}")
        work = self.voice_dir / f"{voice}-enc.wav"
        work.write_bytes(wav.read_bytes())
        codec_path = str(next(iter(self.models.values())).get("codec_path", ""))
        cmd = [codec, "--model", codec_path, "-i", str(work)]
        try:
            out = subprocess.run(
                cmd, capture_output=True, text=True, timeout=120,
                creationflags=CREATE_NO_WINDOW,
            )
            produced = work.with_suffix(".rvq")
            if out.returncode != 0 or not produced.is_file():
                raise OmnivoiceError(
                    f"reference encoding failed ({out.returncode}): {out.stderr[-300:]}"
                )
            produced.replace(rvq)
        finally:
            work.unlink(missing_ok=True)
            work.with_suffix(".rvq").unlink(missing_ok=True)

    def _ensure_process(self, model: str, voice: str, lang: str) -> None:
        """Start or restart the child so it serves this (model, voice, lang)."""
        key = (model, voice, lang)
        if self.running and self._key == key:
            return
        self._stop_child()
        self._log_file = None
        if self.log_dir:
            self.log_dir.mkdir(parents=True, exist_ok=True)
            self._log_file = open(self.log_dir / "omnivoice.log", "ab")
        self._proc = subprocess.Popen(
            self._command(self.models[model], voice, lang),
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=self._log_file or subprocess.DEVNULL,
            creationflags=CREATE_NO_WINDOW,
        )
        self._out = queue.Queue()
        self._reader = threading.Thread(
            target=_pump_stdout, args=(self._proc.stdout.fileno(), self._out), daemon=True
        )
        self._reader.start()
        self._started_at = time.monotonic()
        self._restarts += 1
        self._key = key
        self._last_used = time.monotonic()
        # Weights load lazily on the first line; a probe synthesis would waste
        # one utterance, so the first request pays the load instead.

    def _synthesize_line(self, line: str, timeout_s: float) -> bytes:
        """Write one line, read its utterance(s) until the stream stalls."""
        proc = self._proc
        if proc is None or proc.poll() is not None:
            raise OmnivoiceError("the omnivoice engine is not running")
        try:
            proc.stdin.write((line + "\n").encode("utf-8"))
            proc.stdin.flush()
            wavs = _read_reply(
                self._out, time.monotonic() + timeout_s, self.stall_s, self.quiet_stall_s
            )
        except OmnivoiceError:
            self._stop_child()   # a desynchronised stream cannot be trusted
            raise
        pcm = b"".join(strip_wav_header(w) for w in wavs)
        if not pcm:
            self._stop_child()
            raise OmnivoiceError(f"the engine produced no audio for: {line[:60]!r}")
        return pcm

    def _ensure_idle_watcher(self) -> None:
        if self._idle_task is None or self._idle_task.done():
            self._idle_task = asyncio.create_task(self._idle_watch())

    async def _idle_watch(self) -> None:
        while self.running:
            await asyncio.sleep(15)
            if self._leases > 0 or time.monotonic() - self._last_used <= self.idle_stop_s:
                continue
            async with self._lock:          # never stop under a live request
                if self._leases == 0 and time.monotonic() - self._last_used > self.idle_stop_s:
                    await asyncio.to_thread(self._stop_child)
                    return

    def _stop_child(self) -> None:
        proc, self._proc = self._proc, None
        self._key = None
        self._started_at = None
        reader, self._reader = self._reader, None
        if proc is not None and proc.poll() is None:
            try:
                proc.stdin.close()          # clean EOF: the engine flushes and exits
            except OSError:
                pass
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.kill()
        if reader is not None:
            reader.join(timeout=5)          # exits on EOF once the pipe closes
        while not self._out.empty():        # drop stale bytes before the next child
            self._out.get_nowait()
        if self._log_file is not None:
            try:
                self._log_file.close()
            except OSError:
                pass
            self._log_file = None

    async def stop(self) -> None:
        async with self._lock:
            self._leases = 0
            await asyncio.to_thread(self._stop_child)


# ------------------------------------------------------------------ wav utils
def strip_wav_header(blob: bytes) -> bytes:
    """PCM payload of one streamed utterance (44-byte canonical header)."""
    if len(blob) >= 44 and blob[:4] == b"RIFF" and blob[8:12] == b"WAVE":
        return blob[44:]
    return blob


def join_pcm(chunks: list[bytes]) -> bytes:
    """One 24 kHz mono s16 WAV from concatenated utterance PCM."""
    pcm = b"".join(chunks)
    data_size = len(pcm)
    header = b"RIFF" + struct.pack("<I", 36 + data_size) + b"WAVE"
    header += b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, SAMPLE_RATE, SAMPLE_RATE * 2, 2, 16)
    header += b"data" + struct.pack("<I", data_size)
    return header + pcm


def _read_reply(
    out: "queue.Queue[bytes]", deadline: float,
    stall_s: float = STALL_S, quiet_stall_s: float = QUIET_STALL_S,
) -> list[bytes]:
    """Utterances from the reader queue: split on RIFF magic, stop on a stall.

    The engine writes a fresh 44-byte header per utterance with live-marker
    sizes, so the client cannot trust the header lengths. The stream is split
    on the next "RIFF" that starts after a header (44 bytes in), a quiet pipe
    means the reply to the last line is complete, and b"" (EOF sentinel) ends
    everything. A magic straddling two chunks stays pending in the buffer
    until it is complete. The stall is two-phase: waiting for the first PCM
    byte (header only in the buffer) uses the long threshold, flowing PCM the
    short one.
    """
    blobs: list[bytes] = []
    buf = bytearray()
    last_data = time.monotonic()
    while True:
        if time.monotonic() > deadline:
            raise OmnivoiceError("omnivoice: reply timed out")
        if len(buf) >= 44:
            pos = buf.find(b"RIFF", 44)
            if pos > 0:
                blobs.append(bytes(buf[:pos]))
                del buf[:pos]
        quiet = len(buf) > 44                 # PCM phase of the current utterance
        threshold = quiet_stall_s if quiet else stall_s
        timeout = min(threshold, max(0.02, deadline - time.monotonic()))
        try:
            chunk = out.get(timeout=timeout)
        except queue.Empty:
            if time.monotonic() - last_data >= threshold:
                if buf:
                    blobs.append(bytes(buf))
                return blobs
            continue
        if not chunk:                         # EOF: engine closed its stdout
            if buf:
                blobs.append(bytes(buf))
            return blobs
        buf.extend(chunk)
        last_data = time.monotonic()


def _pump_stdout(fd: int, out: "queue.Queue[bytes]") -> None:
    """Reader thread: raw blocking reads (selectors cannot poll pipes on Windows)."""
    try:
        while True:
            chunk = os.read(fd, 65536)
            out.put(chunk)
            if not chunk:
                return
    except OSError:
        out.put(b"")
