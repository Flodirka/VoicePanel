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


@router.post("/v1/audio/diarize")
async def diarize(
    file: UploadFile = File(...),
    model: str | None = Form(None),
    registry: Registry = Depends(get_registry),
) -> JSONResponse:
    """Who spoke when, for one utterance or one whole recording.

    Returns the engine's speaker turns unchanged: ``start_sample``,
    ``end_sample``, ``speaker_id`` and ``confidence`` from a task-``diar`` model.
    """
    raw = await file.read()
    if not raw:
        raise HTTPException(400, "empty audio upload")
    try:
        if raw[:4] == b"RIFF":
            wav = to_standard_wav(raw)
        else:
            wav = decode_any_audio(raw, file.filename or "audio")
    except ValueError as exc:
        raise HTTPException(400, str(exc))
    try:
        model_id = registry.resolve_diar(model)
    except KeyError as exc:
        raise HTTPException(404, str(exc))
    try:
        turns = await registry.diar_backend(model_id).diarize(wav, model=model_id)
    except httpx.HTTPStatusError as exc:
        raise HTTPException(502, f"diarisation backend error: {exc.response.text[:300]}")
    except httpx.HTTPError as exc:
        raise HTTPException(504, f"diarisation backend unreachable: {exc}")
    return JSONResponse({"model": model_id, "speaker_turns": turns})


def tts_max_tokens(text: str, requested: int | None = None) -> int:
    """Bound one synthesis request.

    Without a cap the models do not stop themselves: measured on this machine,
    Qwen3-TTS produced 131 s of audio for a ten-word sentence and held the
    request for 415 s, and MOSS-TTS-Nano ran to a 48 s ceiling. Frame rates
    differ per model (12.5 Hz for Qwen3-TTS, ~20 Hz for MOSS-TTS-Nano), so the
    bound stays generous: 60 frames plus about 1.2 per UTF-8 byte, clamped to
    [64, 1024]. A caller that knows better can pass its own value.
    """
    if requested and requested > 0:
        return min(int(requested), 4096)
    return max(64, min(1024, 60 + int(len(text.encode("utf-8")) * 1.2)))


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
    requested_model = body.get("model")
    # Server defaults apply only to automatic routing. Explicit models and
    # voices stay under the caller's control. The per-language default may be
    # a voice profile id or a built-in model id (e.g. piper-ru-female to pick
    # the female Russian voice over the male one).
    if not voice and requested_model in (None, "", "default", "tts", "auto"):
        lang = "ru" if has_cyrillic(text) else "en"
        default_voice = registry.cfg.get("tts", {}).get("default_voices", {}).get(lang) or None
        if default_voice:
            if default_voice in registry.tts_model_ids():
                requested_model = default_voice
            elif request.app.state.voices.get(str(default_voice)) is None:
                raise HTTPException(503, f"configured {lang} voice profile '{default_voice}' is unavailable")
            else:
                voice = default_voice
    language = body.get("language")
    speed = body.get("speed")
    response_format = body.get("response_format", "wav")
    options = dict(body.get("options") or {})

    # A named voice profile selects the cloning model and its reference.
    profile = None
    if voice and getattr(request.app.state, "voices", None) is not None:
        profile = request.app.state.voices.get(str(voice))
    if profile is not None:
        # A profile records the clone engine it was created with. That engine may
        # no longer be served (e.g. an audio.cpp helper replaced by Omnivoice), so
        # resolve it to an engine the service actually knows instead of handing a
        # stale id to a backend that answers "unknown model".
        model_id = registry.effective_profile_model(profile.get("model"))
        voice = profile["id"]
        language = language or ("ru" if has_cyrillic(text) else "en")
    else:
        try:
            model_id = registry.resolve_tts(requested_model, text)
        except KeyError as exc:
            raise HTTPException(404, str(exc))
        # A clone model without a per-language built-in needs a reference
        # profile. piper-ru handles everyday Russian; a cloned profile is
        # selected explicitly (voice=<profile-id>).
        clone_id = registry.cfg.get("tts", {}).get("clone_model", "omnivoice-q8")
        if model_id == clone_id and not voice:
            raise HTTPException(
                400,
                f"{clone_id} is a cloning model and needs a voice profile: "
                "create one in the panel (Settings -> Voice profiles) and "
                "select it, or send voice=<profile-id>.",
            )

    # The generation cap applies to the audio-token clone engines only: the
    # VITS built-ins (Kokoro, Piper) reject unknown request options outright.
    if profile is not None or model_id == registry.cfg.get("tts", {}).get("clone_model", "omnivoice-q8"):
        options.setdefault("max_tokens", tts_max_tokens(text, body.get("max_tokens")))

    async with registry._tts_lock:
        backend = registry.tts_backend(model_id)
        try:
            audio = await backend.speech(
                text, model=model_id, voice=voice, speed=speed, language=language, options=options
            )
        except httpx.HTTPStatusError as exc:
            raise HTTPException(502, f"TTS backend error: {exc.response.text[:300]}")
        except httpx.HTTPError as exc:
            raise HTTPException(504, f"TTS backend unreachable: {exc}")
        except RuntimeError as exc:      # the omnivoice engine reports via RuntimeError
            raise HTTPException(502, f"TTS backend error: {exc}")
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
