"""Shared fixtures: temp voice library and WAV builders."""
from __future__ import annotations

import sys
from pathlib import Path

import numpy as np
import pytest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "service"))

from voiceservice.audio import samples_to_wav  # noqa: E402
from voiceservice.voices import VoiceStore  # noqa: E402


def make_wav(seconds: float, sample_rate: int = 24000, freq: float = 220.0) -> bytes:
    """A quiet sine tone of the requested length, as 16-bit mono WAV bytes."""
    n = int(seconds * sample_rate)
    t = np.arange(n, dtype=np.float32) / sample_rate
    samples = 0.1 * np.sin(2 * np.pi * freq * t, dtype=np.float32)
    return samples_to_wav(samples, sample_rate)


@pytest.fixture()
def store(tmp_path: Path) -> VoiceStore:
    return VoiceStore(tmp_path / "voice-library")


@pytest.fixture()
def wav_3s() -> bytes:
    return make_wav(3.0)
