"""Voice profile management API (zero-shot cloning references).

    GET    /v1/voices                     list profiles
    POST   /v1/voices                     create (multipart: name, file, reference_text?, language?, transcribe?)
    GET    /v1/voices/{id}                profile details
    PATCH  /v1/voices/{id}                update name/transcript/reference
    DELETE /v1/voices/{id}                remove
    POST   /v1/voices/{id}/preview        synthesise a sample with this voice

All data stays local: profiles are files under runtime/voice-library/.
"""
from __future__ import annotations

import time
from typing import Any

import httpx
from fastapi import APIRouter, Depends, File, Form, HTTPException, UploadFile
from fastapi.responses import Response

from ..backends import Registry
from ..processing import clean_transcript
from ..voices import VoiceProfileError
from .deps import get_registry, get_voices

router = APIRouter()

PREVIEW_TEXT = {
    "ru": "Привет! Это проверка клонированного голоса. Синтез выполняется локально.",
    "en": "Hello! This is a cloned voice check. Synthesis runs fully locally.",
}


@router.get("/v1/voices")
async def list_voices(store=Depends(get_voices)) -> dict[str, Any]:
    return {"voices": store.list()}


@router.post("/v1/voices")
async def create_voice(
    file: UploadFile = File(...),
    name: str = Form(...),
    reference_text: str = Form(""),
    language: str = Form(""),
    transcribe: bool = Form(False),
    registry: Registry = Depends(get_registry),
    store=Depends(get_voices),
) -> dict[str, Any]:
    audio = await file.read()
    if not audio:
        raise HTTPException(400, "reference audio upload is empty")
    if not reference_text.strip() and transcribe:
        # Use the service's own ASR to draft a transcript; the user can edit it later.
        from ..audio import to_standard_wav

        try:
            async with registry._asr_lock:
                result = await registry.audiocpp.transcribe(
                    to_standard_wav(audio), model=registry.cfg["asr"]["default"]
                )
            reference_text = clean_transcript(result.get("text", ""))
        except Exception as exc:  # noqa: BLE001 - transcription is a convenience
            raise HTTPException(502, f"auto-transcription of the reference failed: {exc}")
    try:
        profile = store.create(
            name=name,
            audio_bytes=audio,
            reference_text=reference_text,
            language=language,
            model=registry.cfg.get("tts", {}).get("clone_model", "qwen3-tts-clone"),
        )
    except VoiceProfileError as exc:
        raise HTTPException(400, str(exc))
    return {"voice": profile, "note": "reference_text improves cloning quality; edit it if the auto transcript is imperfect"}


@router.get("/v1/voices/{voice_id}")
async def get_voice(voice_id: str, store=Depends(get_voices)) -> dict[str, Any]:
    profile = store.get(voice_id)
    if profile is None:
        raise HTTPException(404, f"unknown voice profile '{voice_id}'")
    return {"voice": profile}


@router.patch("/v1/voices/{voice_id}")
async def update_voice(
    voice_id: str,
    name: str | None = Form(None),
    reference_text: str | None = Form(None),
    language: str | None = Form(None),
    model: str | None = Form(None),
    file: UploadFile | None = File(None),
    store=Depends(get_voices),
) -> dict[str, Any]:
    audio = await file.read() if file is not None else None
    try:
        profile = store.update(
            voice_id,
            name=name,
            reference_text=reference_text,
            language=language,
            model=model,
            audio_bytes=audio,
        )
    except VoiceProfileError as exc:
        code = 404 if "unknown profile" in str(exc) else 400
        raise HTTPException(code, str(exc))
    return {"voice": profile}


@router.delete("/v1/voices/{voice_id}")
async def delete_voice(voice_id: str, store=Depends(get_voices)) -> dict[str, Any]:
    if not store.delete(voice_id):
        raise HTTPException(404, f"unknown voice profile '{voice_id}'")
    return {"deleted": voice_id}


@router.post("/v1/voices/{voice_id}/preview")
async def preview_voice(
    voice_id: str,
    registry: Registry = Depends(get_registry),
    store=Depends(get_voices),
) -> Response:
    profile = store.get(voice_id)
    if profile is None:
        raise HTTPException(404, f"unknown voice profile '{voice_id}'")
    language = profile.get("language") or "en"
    text = PREVIEW_TEXT.get(language, PREVIEW_TEXT["en"])
    started = time.perf_counter()
    try:
        audio = await _synth_with_voice(registry, profile, text, language)
    except httpx.HTTPStatusError as exc:
        raise HTTPException(502, f"cloning TTS error: {exc.response.text[:300]}")
    except httpx.HTTPError as exc:
        raise HTTPException(504, f"cloning TTS unreachable: {exc}")
    return Response(
        content=audio,
        media_type="audio/wav",
        headers={"X-Generation-Ms": str(int((time.perf_counter() - started) * 1000))},
    )


async def _synth_with_voice(
    registry: Registry, profile: dict[str, Any], text: str, language: str
) -> bytes:
    """Synthesize with a cloned profile: the cloning model + voice library name."""
    model = profile.get("model") or "qwen3-tts-clone"
    backend = registry.tts_backend(model)
    async with registry._tts_lock:
        return await backend.speech(
            text, model=model, voice=profile["id"], language=language
        )
