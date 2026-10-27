"""OpenAI-compatible endpoints.

* POST /v1/audio/transcriptions  - multipart; Pithagoras-compatible (WAV 16 kHz
  mono PCM, response {"text": "..."}), also accepts other common formats.
* POST /v1/audio/speech          - JSON; local TTS (Kokoro / Piper).
* POST /v1/chat/completions      - proxied to the local LLM (translation/correction
  clients may talk to this directly with their own prompts).
* GET  /v1/models                - logical model list.
"""
from __future__ import annotations

import asyncio
import time
from typing import Any

import httpx
from fastapi import APIRouter, Depends, File, Form, HTTPException, Request, UploadFile
from fastapi.responses import JSONResponse, Response, StreamingResponse

from .. import __version__
from ..audio import decode_any_audio, read_wav_bytes, samples_to_wav, to_standard_wav
from ..backends import Registry, has_cyrillic
from ..processing import clean_transcript
from .deps import get_registry

router = APIRouter()


@router.post("/v1/audio/transcriptions")
async def transcriptions(
    request: Request,
    file: UploadFile = File(...),
    model: str | None = Form(None),
    language: str | None = Form(None),
    prompt: str | None = Form(None),
    response_format: str = Form("json"),
    temperature: float | None = Form(None),
    timestamp_granularities: list[str] | None = Form(None),
    registry: Registry = Depends(get_registry),
):
    raw = await file.read()
    limit = registry.cfg["service"].get("max_upload_mb", 200) * 1024 * 1024
    if len(raw) > limit:
        raise HTTPException(413, f"audio larger than {limit // (1024*1024)} MB")
    if not raw:
        raise HTTPException(400, "empty audio upload")

    # Normalise to WAV 16 kHz mono PCM for every backend.
    name = file.filename or "audio"
    try:
        if name.lower().endswith(".wav") or raw[:4] == b"RIFF":
            wav = to_standard_wav(raw)
        else:
            wav = decode_any_audio(raw, name)
    except ValueError:
        try:
            wav = decode_any_audio(raw, name)
        except ValueError as exc:
            raise HTTPException(400, str(exc))

    try:
        backend, model_id = registry.resolve_asr(model)
    except KeyError as exc:
        raise HTTPException(404, str(exc))

    started = time.perf_counter()
    lock = registry._asr_lock
    segments: list[dict[str, Any]] = []
    detected: str | None = None
    timing: dict[str, Any] = {}
    try:
        if backend == "whisper":
            async with lock:
                result = await registry.whisper.infer(
                    wav, language=language, prompt=prompt,
                    temperature=temperature if temperature is not None else 0.0,
                    verbose=response_format == "verbose_json",
                )
            text = clean_transcript(result.get("text", ""))
            segments = [
                {
                    "id": seg.get("id"),
                    "text": str(seg.get("text", "")).strip(),
                    "start": round(float(seg.get("start", 0.0)), 3),
                    "end": round(float(seg.get("end", 0.0)), 3),
                }
                for seg in (result.get("segments") or [])
                if str(seg.get("text", "")).strip()
            ]
            detected = result.get("language")
        else:
            async with lock:
                if response_format == "verbose_json":
                    result = await registry.audiocpp.transcribe_detailed(
                        wav, model=model_id, language=language
                    )
                else:
                    result = await registry.audiocpp.transcribe(
                        wav, model=model_id, language=language, prompt=prompt
                    )
            text = clean_transcript(result.get("text", ""))
            segments = result.get("segments") or result.get("words") or []
            timing = result.get("timing") or {}
            detected = result.get("language")
    except httpx.HTTPStatusError as exc:
        detail = exc.response.text[:500]
        raise HTTPException(502, f"ASR backend error: {detail}")
    except httpx.HTTPError as exc:
        raise HTTPException(504, f"ASR backend unreachable: {exc}")

    duration_s = round(time.perf_counter() - started, 3)
    if response_format == "text":
        return Response(text, media_type="text/plain; charset=utf-8")
    payload: dict[str, Any] = {"text": text}
    if response_format == "verbose_json":
        samples, sr = read_wav_bytes(wav)
        payload.update(
            language=detected or language or "auto",
            duration=round(len(samples) / sr, 3),
            segments=segments,
            backend=backend,
            model=model_id,
            processing_ms=int(duration_s * 1000),
            **({"timing": timing} if timing else {}),
        )
    return JSONResponse(payload)


def _ts_to_s(ts_ms: Any) -> float:
    return round(int(ts_ms or 0) / 1000.0, 3)


@router.post("/v1/audio/speech")
async def speech(
    request: Request,
    registry: Registry = Depends(get_registry),
) -> Response:
    body = await request.json()
    text = str(body.get("input", "")).strip()
    if not text:
        raise HTTPException(400, "input text is empty")
    voice = body.get("voice")
    language = body.get("language")
    speed = body.get("speed")
    response_format = body.get("response_format", "wav")

    # A named voice profile selects the cloning model and its reference.
    profile = None
    if voice and getattr(request.app.state, "voices", None) is not None:
        profile = request.app.state.voices.get(str(voice))
    if profile is not None:
        model_id = profile.get("model") or "qwen3-tts-clone"
        voice = profile["id"]
        language = language or ("ru" if has_cyrillic(text) else "en")
    else:
        try:
            model_id = registry.resolve_tts(body.get("model"), text)
        except KeyError as exc:
            raise HTTPException(404, str(exc))
        # The only local Russian voice is the cloning model, which needs a
        # reference profile. Built-in Piper builds reject non-English input.
        if model_id == "qwen3-tts-clone" and not voice:
            raise HTTPException(
                400,
                "Russian speech needs a voice profile: create one in the panel "
                "(Settings -> Voice profiles) and select it, or send voice=<profile-id>.",
            )

    async with registry._tts_lock:
        backend = registry.tts_backend(model_id)
        try:
            audio = await backend.speech(
                text, model=model_id, voice=voice, speed=speed, language=language
            )
        except httpx.HTTPStatusError as exc:
            raise HTTPException(502, f"TTS backend error: {exc.response.text[:300]}")
        except httpx.HTTPError as exc:
            raise HTTPException(504, f"TTS backend unreachable: {exc}")
    if response_format == "pcm":
        # strip WAV header for clients that want raw PCM
        samples, sr = read_wav_bytes(audio)
        headers = {"X-Sample-Rate": str(sr)}
        pcm = (samples * 32767.0).astype("<i2").tobytes()
        return Response(pcm, media_type="audio/pcm", headers=headers)
    media = "audio/wav"
    return Response(audio, media_type=media)


@router.post("/v1/chat/completions")
async def chat_completions(
    request: Request,
    registry: Registry = Depends(get_registry),
):
    if registry.llm is None:
        raise HTTPException(503, "LLM backend is disabled in configuration")
    payload = await request.json()
    # Map any model name to the single served local model.
    payload["model"] = registry.llm.model_alias
    payload.setdefault("temperature", 0.2)

    async with registry._llm_semaphore:
        if payload.get("stream"):
            async def gen():
                try:
                    async with registry.llm.request(payload) as resp:
                        async for chunk in resp.aiter_bytes():
                            yield chunk
                except httpx.HTTPError as exc:
                    raise HTTPException(502, f"LLM backend unreachable: {exc}")

            return StreamingResponse(gen(), media_type="text/event-stream")
        try:
            async with registry.llm.request(payload) as resp:
                resp.raise_for_status()
                data = resp.json()
        except httpx.HTTPStatusError as exc:
            raise HTTPException(502, f"LLM backend error: {exc.response.text[:300]}")
        except httpx.HTTPError as exc:
            raise HTTPException(504, f"LLM backend unreachable: {exc}")
    return JSONResponse(data)


@router.get("/v1/models")
async def models(registry: Registry = Depends(get_registry)) -> dict[str, Any]:
    data = []
    for mid in registry.asr_model_ids():
        data.append({"id": mid, "object": "model", "owned_by": "voice-service", "capability": "asr"})
    for mid in registry.tts_model_ids():
        data.append({"id": mid, "object": "model", "owned_by": "voice-service", "capability": "tts"})
    if registry.llm is not None:
        data.append({
            "id": registry.llm.model_alias,
            "object": "model",
            "owned_by": "voice-service",
            "capability": "llm",
        })
    return {"object": "list", "data": data}
