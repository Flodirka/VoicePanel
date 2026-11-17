"""OmnivoiceBackend: the piped-CLI protocol against a fake engine child.

The real engine (k2-fsa omnivoice.cpp) synthesizes one utterance per stdin
line: a fresh 44-byte RIFF header with live-marker sizes, then PCM, flushed
immediately. The fake below mimics that byte stream exactly (including one
utterance per line and a multi-utterance line) so the supervisor logic —
stream splitting, stalls, restarts, reference caching — is tested offline.
"""
from __future__ import annotations

import asyncio
import json
import os
import struct
import sys
from pathlib import Path

import pytest
import httpx

from voiceservice.backends.omnivoice import OmnivoiceBackend, OmnivoiceHttpBackend, OmnivoiceError


def test_default_http_backend_ports_do_not_collide() -> None:
    from voiceservice.config import DEFAULTS

    ports = [entry["port"] for entry in DEFAULTS["backends"].values() if "port" in entry]
    assert len(ports) == len(set(ports))

FAKE_ENGINE = r"""
import struct, sys

# argv: --emit <n utterances per line>
emit = 1
for i, a in enumerate(sys.argv):
    if a == "--emit":
        emit = int(sys.argv[i + 1])

def wav_header():
    return (b"RIFF" + struct.pack("<I", 0x7FFFFFFF) + b"WAVE"
            + b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, 24000, 48000, 2, 16)
            + b"data" + struct.pack("<I", 0x7FFFFFFF))

def pcm(tag):
    return tag.encode() + b"\x00\x01" * 64

for line in sys.stdin:
    line = line.strip()
    if not line:
        continue
    for k in range(emit):
        out = wav_header() + pcm(f"{line}#{k}")
        sys.stdout.buffer.write(out)
        sys.stdout.buffer.flush()
"""


def make_backend(tmp_path: Path, emit: int = 1, **overrides) -> OmnivoiceBackend:
    script = tmp_path / "fake_engine.py"
    script.write_text(FAKE_ENGINE, encoding="utf-8")
    # The engine binary is launched directly, so the fake needs an executable
    # shim per platform: Windows cannot exec a text file as an .exe (a .bat
    # wrapper works), POSIX runs a shebang script after chmod +x.
    if sys.platform == "win32":
        engine = tmp_path / "omnivoice-tts.bat"
        engine.write_text(
            f'@echo off\r\n"{sys.executable}" "{script}" --emit {emit} %*\r\n',
            encoding="utf-8",
        )
    else:
        engine = tmp_path / "omnivoice-tts"
        engine.write_text(
            f"#!{sys.executable}\n"
            "import runpy, sys\n"
            f"sys.argv = [sys.argv[0], '--emit', '{emit}'] + sys.argv[1:]\n"
            f"runpy.run_path({str(script)!r}, run_name='__main__')\n",
            encoding="utf-8",
        )
        engine.chmod(0o755)
    codec = tmp_path / "omnivoice-codec.exe"
    codec.write_text("", encoding="utf-8")
    models_dir = tmp_path / "models"
    models_dir.mkdir(exist_ok=True)
    (models_dir / "base.gguf").write_bytes(b"")
    (models_dir / "codec.gguf").write_bytes(b"")
    cfg = {
        "enabled": True,
        "binary": str(engine),
        "codec_binary": str(codec),
        "voice_dir": str(tmp_path / "voice-library"),
        "idle_stop_s": 3600,
        "request_timeout_s": 30,
        "max_line_chars": 300,
        "steps": 4,
        "stall_s": 3.0,        # short stalls keep the fake-engine tests fast
        "quiet_stall_s": 0.4,
        "models": [{
            "id": "omnivoice-q8",
            "family": "omnivoice",
            "path": str(models_dir / "base.gguf"),
            "codec_path": str(models_dir / "codec.gguf"),
            "task": "tts",
        }],
    }
    cfg.update(overrides)
    return OmnivoiceBackend(cfg, log_dir=tmp_path / "logs")


def test_two_requests_yield_valid_wavs(tmp_path: Path) -> None:
    async def run() -> None:
        backend = make_backend(tmp_path)
        wav1 = await backend.speech("Первая фраза.", model="omnivoice-q8")
        assert wav1[:4] == b"RIFF" and wav1[8:12] == b"WAVE"
        # PCM body = the tag the fake echoes (line + "#0") + 2*64 PCM bytes.
        assert len(wav1) == 44 + (len("Первая фраза.#0".encode()) + 128)
        wav2 = await backend.speech("Вторая.", model="omnivoice-q8")
        assert len(wav2) == 44 + (len("Вторая.#0".encode()) + 128)
        assert backend.running               # the child stays warm between requests
        await backend.stop()
        assert not backend.running

    asyncio.run(run())


def test_neutral_speed_from_panel_is_accepted(tmp_path: Path) -> None:
    async def run() -> None:
        backend = make_backend(tmp_path)
        try:
            wav = await backend.speech("Hello.", model="omnivoice-q8", speed=1.0)
            assert wav[:4] == b"RIFF"
        finally:
            await backend.stop()
    asyncio.run(run())


def test_http_keeps_the_model_when_voice_language_or_design_changes(tmp_path: Path) -> None:
    cfg = {**make_backend(tmp_path).cfg, "binary": str(tmp_path / "tts-server.exe"), "port": 8106}
    backend = OmnivoiceHttpBackend(cfg, tmp_path / "logs")
    backend.proc._owned_and_healthy = lambda: True
    backend.voice_dir.mkdir()
    for name in ("one", "two"):
        (backend.voice_dir / f"{name}.wav").write_bytes(b"reference")
        (backend.voice_dir / f"{name}.rvq").write_bytes(b"cached codes")
    (backend.voice_dir / "prompt_text").write_text("one|First reference.\ntwo|Second reference.", encoding="utf-8")
    requests = []
    def handle(request):
        requests.append((request.url.path, json.loads(request.content)))
        return httpx.Response(200, content=b"RIFF-test")

    async def run():
        async with httpx.AsyncClient(transport=httpx.MockTransport(handle)) as client:
            backend.proc.http = lambda: client
            process = backend.proc
            await backend.speech("First.", model="omnivoice-q8", voice="one", language="en")
            await backend.speech("Second.", model="omnivoice-q8", voice="two", language="ru")
            await backend.speech("Third.", model="omnivoice-q8", voice="one", language="ru")
            await backend.speech("Design.", model="omnivoice-q8", options={"instruct": "female", "seed": 42})
            assert backend.proc is process
            assert len([p for p, _ in requests if p.endswith("/voices")]) == 2
            assert requests[-1][1]["instructions"] == "female"
            assert requests[-1][1]["seed"] == 42
            assert backend.status()["leases"] == 0
            with pytest.raises(OmnivoiceError, match="unsupported"):
                await backend.speech("Steps.", model="omnivoice-q8", options={"steps": 8})
            await backend.stop()
    asyncio.run(run())


def test_reference_cache_is_rebuilt_only_when_audio_changes(tmp_path: Path, monkeypatch) -> None:
    backend = make_backend(tmp_path)
    backend.voice_dir.mkdir()
    wav = backend.voice_dir / "updated.wav"
    rvq = backend.voice_dir / "updated.rvq"
    wav.write_bytes(b"new reference")
    rvq.write_bytes(b"old codes")
    os.utime(rvq, (1, 1))
    (backend.voice_dir / "prompt_text").write_text("updated|Reference.", encoding="utf-8")
    encodings = []
    def encode(voice, audio, codes):
        encodings.append(voice)
        codes.write_bytes(b"new codes")
    monkeypatch.setattr(backend, "_encode_reference", encode)
    backend._reference_args("updated")
    backend._reference_args("updated")
    assert encodings == ["updated"]
    assert rvq.read_bytes() == b"new codes"


def test_design_options_change_process_and_are_not_reused(tmp_path: Path) -> None:
    async def run() -> None:
        backend = make_backend(tmp_path)
        try:
            options = {"instruct": "female, low pitch", "steps": 8, "seed": 42}
            await backend.speech("Hello.", model="omnivoice-q8", options=options)
            args = backend._proc.args
            assert args[args.index("--instruct") + 1] == "female, low pitch"
            assert args[args.index("--steps") + 1] == "8"
            assert args[args.index("--seed") + 1] == "42"
            first_pid = backend._proc.pid
            await backend.speech("Again.", model="omnivoice-q8", options=options)
            assert backend._proc.pid == first_pid
            await backend.speech("Changed.", model="omnivoice-q8", options={**options, "instruct": "male"})
            assert backend._proc.pid != first_pid
            await backend.speech("Default.", model="omnivoice-q8")
            assert "--instruct" not in backend._proc.args
        finally:
            await backend.stop()
    asyncio.run(run())


def test_multi_sentence_text_concatenates_utterances(tmp_path: Path) -> None:
    async def run() -> None:
        backend = make_backend(tmp_path, emit=2)   # the fake answers two utterances per line
        # One sentence (the backend appends the period): one line out, two
        # utterances back, PCM concatenated in order.
        text = "Одно предложение без точки"
        wav = await backend.speech(text, model="omnivoice-q8")
        body = wav[44:]
        expected = b"".join(
            f"{text}.#{k}".encode() + b"\x00\x01" * 64 for k in range(2)
        )
        assert body == expected
        await backend.stop()

    asyncio.run(run())


def test_voice_switch_restarts_the_child(tmp_path: Path) -> None:
    async def run() -> None:
        backend = make_backend(tmp_path)
        library = Path(backend.voice_dir)
        library.mkdir(parents=True, exist_ok=True)
        (library / "a.wav").write_bytes(b"RIFF")
        (library / "prompt_text").write_text("a|transcript a\n", encoding="utf-8")
        (library / "a.rvq").write_bytes(b"codes")   # pre-encoded reference already cached
        await backend.speech("Первый.", model="omnivoice-q8")
        first_pid = backend.status()["pid"]
        await backend.speech("Второй.", model="omnivoice-q8", voice="a")
        assert backend.status()["pid"] != first_pid  # reference args changed: restarted
        same_pid = backend.status()["pid"]
        await backend.speech("Третий.", model="omnivoice-q8", voice="a")
        assert backend.status()["pid"] == same_pid   # same voice: reused
        await backend.stop()

    asyncio.run(run())


def test_profile_without_transcript_fails(tmp_path: Path) -> None:
    async def run() -> None:
        backend = make_backend(tmp_path)
        library = Path(backend.voice_dir)
        library.mkdir(parents=True, exist_ok=True)
        (library / "b.wav").write_bytes(b"RIFF")    # no prompt_text entry
        (library / "b.rvq").write_bytes(b"codes")   # reference already encoded
        with pytest.raises(OmnivoiceError, match="transcript"):
            await backend.speech("Текст.", model="omnivoice-q8", voice="b")
        await backend.stop()

    asyncio.run(run())


def test_empty_text_is_rejected(tmp_path: Path) -> None:
    async def run() -> None:
        backend = make_backend(tmp_path)
        with pytest.raises(OmnivoiceError, match="empty"):
            await backend.speech("   ", model="omnivoice-q8")
        await backend.stop()

    asyncio.run(run())
