"""OmnivoiceBackend: the piped-CLI protocol against a fake engine child.

The real engine (k2-fsa omnivoice.cpp) synthesizes one utterance per stdin
line: a fresh 44-byte RIFF header with live-marker sizes, then PCM, flushed
immediately. The fake below mimics that byte stream exactly (including one
utterance per line and a multi-utterance line) so the supervisor logic —
stream splitting, stalls, restarts, reference caching — is tested offline.
"""
from __future__ import annotations

import asyncio
import struct
import sys
from pathlib import Path

import pytest

from voiceservice.backends.omnivoice import OmnivoiceBackend, OmnivoiceError

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
