"""Session warm-up: load the models a dictation session is about to use.

    POST /v1/warmup   {"asr": true, "llm": true, "mode": "auto_correct",
                       "profile": "general", "context": "...", "dictionary": [...]}

The panel calls this when a dictation session starts. The user speaks for at
least a second before the first utterance ends, so loading the ASR model and
prefilling the text model's prompt prefix in that window removes the cold start
from the first sentence instead of adding it to the first insertion.

The work runs in the background and the route answers 202 at once. Nothing is
done for a model that is already resident. An external text server (an agent's
llama.cpp, for instance) is never warmed: its slots and prompt cache belong to
another application.
"""
from __future__ import annotations

import asyncio
import logging
from typing import Any

import numpy as np
from fastapi import APIRouter, Depends
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field

from ..audio import samples_to_wav
from ..backends import Registry
from ..processing import PROFILES, build_messages
from .deps import get_registry

router = APIRouter()
log = logging.getLogger("voiceservice.warmup")

# Panel modes include the automatic ones; any correction or translation mode
# warms the same system prefix, because the task line comes last.
_LLM_MODES = {
    "auto_correct": "correct_en",
    "auto_translate": "translate_ru_en",
    "correct_en": "correct_en",
    "correct_ru": "correct_ru",
    "translate_ru_en": "translate_ru_en",
    "translate_en_ru": "translate_en_ru",
}
_SILENCE_WAV = samples_to_wav(np.zeros(8000, dtype=np.float32))   # 0.5 s


class WarmupRequest(BaseModel):
    asr: bool = True
    llm: bool = True
    mode: str | None = None
    profile: str = "general"
    context: str | None = Field(default=None, max_length=64000)
    dictionary: list[str] | None = None


@router.post("/v1/warmup")
async def warmup(req: WarmupRequest, registry: Registry = Depends(get_registry)) -> JSONResponse:
    scheduled: list[str] = []
    if req.asr and _schedule(registry, "asr", _warm_asr(registry)):
        scheduled.append("asr")
    mode = _LLM_MODES.get(req.mode or "")
    if (
        req.llm
        and mode is not None
        and registry.llm is not None
        and not registry.llm.external
        and _schedule(registry, "llm", _warm_llm(registry, req, mode))
    ):
        scheduled.append("llm")
    return JSONResponse({"scheduled": scheduled}, status_code=202)


def _schedule(registry: Registry, key: str, coro) -> bool:
    """Run one warm-up per kind at a time; a second call while it runs is a no-op."""
    if key in registry.warming:
        coro.close()
        return False
    registry.warming.add(key)

    async def run() -> None:
        try:
            await coro
        except Exception as exc:  # noqa: BLE001 - a failed warm-up is not an error for the caller
            log.info("warm-up of %s skipped: %s", key, exc)
        finally:
            registry.warming.discard(key)

    task = asyncio.create_task(run())
    registry.background_tasks.add(task)
    task.add_done_callback(registry.background_tasks.discard)
    return True


async def _warm_asr(registry: Registry) -> None:
    backend, model_id = registry.resolve_asr(None)
    if backend != "audiocpp":
        return   # whisper.cpp loads its model at process start; leasing is enough
    audiocpp = registry.audiocpp

    async def resident() -> bool:
        if not audiocpp.proc.running:
            return False
        loaded = {m.get("id"): m for m in await audiocpp.loaded_models()}
        return bool(loaded.get(model_id, {}).get("loaded"))

    if await resident():
        return
    async with registry._asr_lock:
        # A real request that held the lock before us may have loaded the
        # model already; then the silent request would only delay the next one.
        if await resident():
            return
        await audiocpp.transcribe(_SILENCE_WAV, model=model_id, timeout=120.0)


async def _warm_llm(registry: Registry, req: WarmupRequest, mode: str) -> None:
    llm = registry.llm
    if llm is None:
        return
    profile = req.profile if req.profile in PROFILES else "general"
    messages: list[dict[str, Any]] = build_messages(
        "OK.", mode=mode, profile=profile, dictionary=req.dictionary, context=req.context
    )
    async with registry._llm_semaphore:
        # One generated token: enough to load the weights and leave the shared
        # system prefix in the slot's KV cache for the first real sentence.
        await llm.complete(
            messages,
            max_tokens=1,
            temperature=0.0,
            chat_template_kwargs={"enable_thinking": False},
        )
