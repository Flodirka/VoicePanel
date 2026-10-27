"""Audio helpers: normalisation to 16 kHz mono PCM WAV (the interchange format
used with every backend, and the format Pithagoras sends)."""
from __future__ import annotations

import io
import wave

import numpy as np

TARGET_SR = 16000


def read_wav_bytes(data: bytes) -> tuple[np.ndarray, int]:
    """Decode WAV bytes to (float32 mono samples, sample_rate)."""
    with wave.open(io.BytesIO(data), "rb") as w:
        n_channels = w.getnchannels()
        sampwidth = w.getsampwidth()
        sr = w.getframerate()
        frames = w.readframes(w.getnframes())
    if sampwidth == 2:
        samples = np.frombuffer(frames, dtype="<i2").astype(np.float32) / 32768.0
    elif sampwidth == 4:
        samples = np.frombuffer(frames, dtype="<i4").astype(np.float32) / 2147483648.0
    elif sampwidth == 1:
        samples = (np.frombuffer(frames, dtype=np.uint8).astype(np.float32) - 128.0) / 128.0
    elif sampwidth == 3:
        raw = np.frombuffer(frames, dtype=np.uint8).reshape(-1, 3)
        samples = (
            raw[:, 0].astype(np.int32)
            | (raw[:, 1].astype(np.int32) << 8)
            | (raw[:, 2].astype(np.int32) << 16)
        ).astype(np.float32) / 8388608.0
        # sign-extend 24-bit values
        samples = np.where(samples >= 1.0, samples - 2.0, samples)
    else:
        raise ValueError(f"unsupported WAV sample width: {sampwidth * 8} bit")
    if n_channels > 1:
        samples = samples.reshape(-1, n_channels).mean(axis=1)
    return samples.astype(np.float32), sr


def resample(samples: np.ndarray, src_sr: int, dst_sr: int = TARGET_SR) -> np.ndarray:
    if src_sr == dst_sr:
        return samples
    gcd = np.gcd(src_sr, dst_sr)
    up, down = dst_sr // gcd, src_sr // gcd
    try:
        from scipy.signal import resample_poly  # type: ignore
        return resample_poly(samples, up, down).astype(np.float32)
    except Exception:
        # linear interpolation fallback (good enough for speech)
        duration = len(samples) / src_sr
        n_out = int(round(duration * dst_sr))
        if n_out == 0:
            return samples[:0]
        x_src = np.linspace(0.0, duration, len(samples), endpoint=False)
        x_dst = np.linspace(0.0, duration, n_out, endpoint=False)
        return np.interp(x_dst, x_src, samples).astype(np.float32)


def samples_to_wav(samples: np.ndarray, sample_rate: int = TARGET_SR) -> bytes:
    """Encode float32 mono samples as 16-bit PCM WAV bytes."""
    pcm = np.clip(samples, -1.0, 1.0)
    pcm = (pcm * 32767.0).astype("<i2")
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm.tobytes())
    return buf.getvalue()


def to_standard_wav(data: bytes, source_format: str | None = None) -> bytes:
    """Normalise arbitrary WAV bytes (any rate/width/channels) to 16 kHz mono PCM16."""
    samples, sr = read_wav_bytes(data)
    samples = resample(samples, sr)
    return samples_to_wav(samples)


def pcm16_to_wav(pcm: bytes, sample_rate: int = TARGET_SR) -> bytes:
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm)
    return buf.getvalue()


def decode_any_audio(data: bytes, filename: str = "") -> bytes:
    """Best-effort decode of common audio formats to standard WAV via soundfile."""
    try:
        import soundfile as sf

        samples, sr = sf.read(io.BytesIO(data), dtype="float32", always_2d=False)
        if samples.ndim > 1:
            samples = samples.mean(axis=1)
        return samples_to_wav(resample(samples.astype(np.float32), sr))
    except Exception as exc:
        raise ValueError(
            f"could not decode audio '{filename}' ({exc}); supply WAV/FLAC/OGG/MP3"
        ) from exc
