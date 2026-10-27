"""Voice profiles: named zero-shot cloning references stored locally.

A profile is a short reference recording plus its transcript. Profiles live in
``runtime/voice-library/`` in the format audio.cpp's voice library understands:

    voice-library/
      <id>.wav        reference recording (16-24 kHz mono recommended, 5-20 s)
      prompt_text     one line per voice: "<id>|<exact transcript>"

The TTS server reads the WAV at request time, so creating, updating, or deleting
a profile needs no backend restart. Conditioning (speaker embedding + codec
prompt) is cached server-side via the model's ``voice_prompt_cache_slots``
session option.

Supported by cloning-capable models (verified families):
  * qwen3_tts (Base variant) - 10 languages incl. ru/en, needs the transcript
    unless ``x_vector_only_mode`` is used
  * chatterbox - multilingual clone, reference via voice library as well

The manifest adds bookkeeping on top (display name, source language, dates).
"""
from __future__ import annotations

import json
import re
import time
import uuid
from pathlib import Path
from typing import Any

from .audio import read_wav_bytes, samples_to_wav

SAFE_ID = re.compile(r"^[a-z0-9][a-z0-9_-]{0,63}$")
# Practical bounds for a usable zero-shot reference.
MIN_REFERENCE_S = 2.0
MAX_REFERENCE_S = 60.0


class VoiceProfileError(ValueError):
    pass


class VoiceStore:
    def __init__(self, library_dir: Path) -> None:
        self.dir = library_dir
        self.dir.mkdir(parents=True, exist_ok=True)
        self._ensure_prompt_text()

    # ------------------------------------------------------------------ files
    def _ensure_prompt_text(self) -> None:
        if not (self.dir / "prompt_text").exists():
            (self.dir / "prompt_text").write_text("", encoding="utf-8")

    @property
    def manifest_path(self) -> Path:
        return self.dir / "manifest.json"

    def _load_manifest(self) -> dict[str, dict[str, Any]]:
        if not self.manifest_path.is_file():
            return {}
        try:
            return json.loads(self.manifest_path.read_text(encoding="utf-8"))
        except json.JSONDecodeError:
            return {}

    def _save_manifest(self, manifest: dict[str, dict[str, Any]]) -> None:
        tmp = self.manifest_path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(manifest, indent=2, ensure_ascii=False), encoding="utf-8")
        tmp.replace(self.manifest_path)

    def _write_prompt_text(self, manifest: dict[str, dict[str, Any]]) -> None:
        lines = []
        for vid in sorted(manifest):
            entry = manifest[vid]
            if entry.get("reference_text"):
                lines.append(f"{vid}|{entry['reference_text']}")
        tmp = self.dir / "prompt_text.tmp"
        tmp.write_text("\n".join(lines) + ("\n" if lines else ""), encoding="utf-8")
        tmp.replace(self.dir / "prompt_text")

    # ------------------------------------------------------------------- CRUD
    def list(self) -> list[dict[str, Any]]:
        manifest = self._load_manifest()
        out = []
        for vid, entry in manifest.items():
            wav = self.dir / f"{vid}.wav"
            out.append({
                "id": vid,
                "name": entry.get("name", vid),
                "model": entry.get("model", "qwen3-tts-clone"),
                "language": entry.get("language", ""),
                "reference_text": entry.get("reference_text", ""),
                "reference_duration_s": entry.get("reference_duration_s"),
                "created_at": entry.get("created_at"),
                "updated_at": entry.get("updated_at"),
                "wav_present": wav.is_file(),
            })
        return sorted(out, key=lambda p: p["name"].lower())

    def get(self, voice_id: str) -> dict[str, Any] | None:
        for profile in self.list():
            if profile["id"] == voice_id:
                return profile
        return None

    def create(
        self,
        *,
        name: str,
        audio_bytes: bytes,
        reference_text: str = "",
        language: str = "",
        model: str = "qwen3-tts-clone",
    ) -> dict[str, Any]:
        voice_id = _slug(name) or f"voice-{uuid.uuid4().hex[:8]}"
        if not SAFE_ID.match(voice_id):
            raise VoiceProfileError(f"invalid profile name '{name}'")
        if (self.dir / f"{voice_id}.wav").exists():
            raise VoiceProfileError(f"profile '{voice_id}' already exists")

        wav = _normalise_reference(audio_bytes)
        duration = _wav_duration_s(wav)
        if duration < MIN_REFERENCE_S:
            raise VoiceProfileError(
                f"reference too short: {duration:.1f}s, need at least {MIN_REFERENCE_S}s"
            )
        if duration > MAX_REFERENCE_S:
            raise VoiceProfileError(
                f"reference too long: {duration:.1f}s, keep it under {MAX_REFERENCE_S}s"
            )

        manifest = self._load_manifest()
        manifest[voice_id] = {
            "name": name.strip() or voice_id,
            "model": model,
            "language": language,
            "reference_text": reference_text.strip(),
            "reference_duration_s": round(duration, 2),
            "created_at": int(time.time()),
            "updated_at": int(time.time()),
        }
        (self.dir / f"{voice_id}.wav").write_bytes(wav)
        self._write_prompt_text(manifest)
        self._save_manifest(manifest)
        return self.get(voice_id)  # type: ignore[return-value]

    def update(
        self,
        voice_id: str,
        *,
        name: str | None = None,
        reference_text: str | None = None,
        language: str | None = None,
        model: str | None = None,
        audio_bytes: bytes | None = None,
    ) -> dict[str, Any]:
        manifest = self._load_manifest()
        if voice_id not in manifest:
            raise VoiceProfileError(f"unknown profile '{voice_id}'")
        entry = manifest[voice_id]
        if name is not None:
            entry["name"] = name.strip() or entry["name"]
        if reference_text is not None:
            entry["reference_text"] = reference_text.strip()
        if language is not None:
            entry["language"] = language
        if model is not None:
            entry["model"] = model
        if audio_bytes is not None:
            wav = _normalise_reference(audio_bytes)
            duration = _wav_duration_s(wav)
            if duration < MIN_REFERENCE_S or duration > MAX_REFERENCE_S:
                raise VoiceProfileError(
                    f"reference duration {duration:.1f}s outside {MIN_REFERENCE_S}-{MAX_REFERENCE_S}s"
                )
            (self.dir / f"{voice_id}.wav").write_bytes(wav)
            entry["reference_duration_s"] = round(duration, 2)
        entry["updated_at"] = int(time.time())
        self._write_prompt_text(manifest)
        self._save_manifest(manifest)
        return self.get(voice_id)  # type: ignore[return-value]

    def delete(self, voice_id: str) -> bool:
        manifest = self._load_manifest()
        if voice_id not in manifest:
            return False
        del manifest[voice_id]
        (self.dir / f"{voice_id}.wav").unlink(missing_ok=True)
        self._write_prompt_text(manifest)
        self._save_manifest(manifest)
        return True


def _slug(name: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", name.strip().lower()).strip("-")
    if not SAFE_ID.match(slug):
        return ""
    return slug


def _normalise_reference(audio_bytes: bytes) -> bytes:
    """Decode any upload to 24 kHz mono PCM WAV (good quality for references)."""
    try:
        samples, sr = read_wav_bytes(audio_bytes)
    except Exception:
        samples, sr = _sf_read(audio_bytes)
    from .audio import resample as _rs

    resampled = _rs(samples, sr, 24000)
    return samples_to_wav(resampled, 24000)

def _sf_read(audio_bytes: bytes):
    import io

    import soundfile as sf

    samples, sr = sf.read(io.BytesIO(audio_bytes), dtype="float32", always_2d=False)
    if samples.ndim > 1:
        samples = samples.mean(axis=1)
    return samples.astype("float32"), sr


def _wav_duration_s(wav_bytes: bytes) -> float:
    samples, sr = read_wav_bytes(wav_bytes)
    return len(samples) / sr if sr else 0.0
