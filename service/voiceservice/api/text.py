"""Text processing endpoint: translation and correction with profiles.

Distinct from /v1/chat/completions: clients pick a *mode* and optional *profile*
and get deterministic, identifier-preserving processing without writing prompts.
"""
from __future__ import annotations

import re
import time
from typing import Any

from fastapi import APIRouter, Depends, HTTPException
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field

from ..backends import Registry
from ..processing import MODES, PROFILES, build_messages
from .deps import get_registry

router = APIRouter()


class SpellingHint(BaseModel):
    word: str = Field(min_length=1, max_length=64)
    suggestions: list[str] = Field(default_factory=list)


class ProcessRequest(BaseModel):
    text: str = Field(min_length=1, max_length=32000)
    mode: str
    profile: str = "general"
    dictionary: list[str] | None = None
    context: str | None = None
    previous: str | None = None
    spelling: list[SpellingHint] | None = None
    temperature: float = 0.2
    max_tokens: int | None = None
    explain: bool = False


@router.get("/v1/text/modes")
async def modes() -> dict[str, Any]:
    return {"modes": list(MODES), "profiles": {k: v for k, v in PROFILES.items()}}


@router.post("/v1/text/process")
async def process_text(
    req: ProcessRequest,
    registry: Registry = Depends(get_registry),
) -> JSONResponse:
    if req.mode not in MODES:
        raise HTTPException(400, f"unknown mode '{req.mode}'; use one of {', '.join(MODES)}")
    if req.profile not in PROFILES:
        raise HTTPException(400, f"unknown profile '{req.profile}'; use one of {', '.join(PROFILES)}")
    if req.mode == "dictation":
        return JSONResponse(
            {"mode": "dictation", "profile": req.profile, "original": req.text, "result": req.text.strip(), "model": None}
        )
    if registry.llm is None:
        raise HTTPException(503, "LLM backend is disabled in configuration")

    messages = build_messages(
        req.text,
        mode=req.mode,
        profile=req.profile,
        dictionary=req.dictionary,
        context=req.context,
        previous=req.previous,
        spelling=[hint.model_dump() for hint in req.spelling] if req.spelling else None,
    )
    if req.explain and req.mode in ("correct_en", "correct_ru"):
        messages[0]["content"] += (
            "\nAfter the corrected text, output a line '---' and then a short list of"
            " the corrections you made (grammar, wording, punctuation). If none, write"
            " 'No changes needed' after the divider."
        )

    started = time.perf_counter()
    try:
        async with registry._llm_semaphore:
            data = await registry.llm.complete(
                messages,
                temperature=req.temperature,
                max_tokens=req.max_tokens,
                # Qwen3.5 reasons before answering unless told otherwise: that
                # put 20 s of thinking and an empty result behind every sentence.
                chat_template_kwargs={"enable_thinking": False},
            )
    except Exception as exc:  # noqa: BLE001 - surfaced as 502 with detail
        raise HTTPException(502, f"text processing failed: {exc}")

    content = (data.get("choices") or [{}])[0].get("message", {}).get("content", "")
    result, notes = content, ""
    if req.explain and "---" in content:
        result, _, notes = content.partition("---")
        result, notes = result.strip(), notes.strip()
    # Models occasionally wrap output in quotes or mark identifiers with
    # backticks; dictated text goes straight into other applications, where
    # neither belongs.
    result = _strip_wrapping_quotes(result.replace("`", "").strip())

    return JSONResponse(
        {
            "mode": req.mode,
            "profile": req.profile,
            "original": req.text,
            "result": result,
            **({"notes": notes} if notes else {}),
            "model": registry.llm.model_alias,
            "duration_ms": int((time.perf_counter() - started) * 1000),
            "usage": data.get("usage"),
        }
    )


def _strip_wrapping_quotes(text: str) -> str:
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'«“”‘’":
        inner = text[1:-1].strip()
        return inner or text
    return text
