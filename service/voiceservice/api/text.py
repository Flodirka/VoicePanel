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
from ..processing import LOQIRA_GEC_PROMPT, MODES, PROFILES, build_messages, deterministic_pass
from .deps import get_registry

router = APIRouter()

CORRECT_MODES = ("correct_en", "correct_ru")


class SpellingHint(BaseModel):
    word: str = Field(min_length=1, max_length=64)
    suggestions: list[str] = Field(default_factory=list)
    proposed: str = ""   # the checker's own REPLACE correction, when it has one


class ProcessRequest(BaseModel):
    text: str = Field(min_length=1, max_length=32000)
    mode: str
    profile: str = "general"
    dictionary: list[str] | None = None
    context: str | None = None
    previous: str | None = None
    spelling: list[SpellingHint] | None = None
    temperature: float | None = None
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
    llm = registry.llm_for(req.mode)
    if llm is None:
        raise HTTPException(503, "LLM backend is disabled in configuration")

    # Deterministic pre-pass: near-misses of the user's dictionary terms and
    # spell-checker hits that agree with the dictionary are fixed without the
    # model (~1 ms). The model then only polishes what is left.
    spelling_dicts = [hint.model_dump() for hint in req.spelling] if req.spelling else None
    text_for_model, dict_changes = deterministic_pass(req.text, dictionary=req.dictionary, spelling=spelling_dicts)
    notes_bits = [f"{heard} -> {fixed}" for heard, fixed in dict_changes]

    # Skip-clean gate: a correction-mode sentence with no dictionary fixes and
    # no spell-checker flags is already as good as the ASR produced, so with
    # llm.skip_if_clean (default on) the model is not called at all. Grammar
    # errors the spell checker cannot see are the accepted trade-off; turn the
    # option off to always run the model.
    skip_if_clean = bool(registry.cfg.get("llm", {}).get("skip_if_clean", True))
    if req.mode in CORRECT_MODES and skip_if_clean and not dict_changes and not req.spelling:
        return JSONResponse(
            {
                "mode": req.mode,
                "profile": req.profile,
                "original": req.text,
                "result": req.text.strip(),
                **({"notes": "; ".join(notes_bits)} if notes_bits else {"notes": "clean"}),
                "model": None,
                "duration_ms": 0,
            }
        )

    # Template-native models (PLaMo's PFML) take context/glossary as chat-template
    # kwargs and echo a leading system turn into the output, so for them the
    # system prompt is dropped and the context document + dictionary travel as
    # `context` / `glossary` kwargs instead. Enabled per backend via
    # `template_style: plamo` in the backend config.
    plamo_style = str(llm.cfg.get("template_style", "")).lower() == "plamo"
    gec_style = llm.cfg.get("template_style") == "loqira_gec" and req.mode in CORRECT_MODES and not req.explain
    if gec_style:
        messages = [
            {"role": "system", "content": LOQIRA_GEC_PROMPT},
            {"role": "user", "content": text_for_model},
        ]
    elif plamo_style:
        messages = [{"role": "user", "content": text_for_model}]
    else:
        messages = build_messages(
            text_for_model,
            mode=req.mode,
            profile=req.profile,
            dictionary=req.dictionary,
            context=req.context,
            previous=req.previous,
            spelling=spelling_dicts,
        )
    if req.explain and req.mode in ("correct_en", "correct_ru") and not plamo_style:
        messages[0]["content"] += (
            "\nAfter the corrected text, output a line '---' and then a short list of"
            " the corrections you made (grammar, wording, punctuation). If none, write"
            " 'No changes needed' after the divider."
        )

    # Correction is a minimal-edit task: sampling only adds invented rewrites.
    if req.temperature is None:
        temperature = 0.0 if req.mode in ("correct_en", "correct_ru") else 0.2
    else:
        temperature = req.temperature

    started = time.perf_counter()
    # Translation models with a native template (PLaMo's PFML) need the target
    # language as a chat-template kwarg; other templates ignore unknown kwargs.
    ctk = {"enable_thinking": False}
    if req.mode == "translate_ru_en":
        ctk["target_lang"] = "en"
    elif req.mode == "translate_en_ru":
        ctk["target_lang"] = "ru"
    if plamo_style:
        # PLaMo's documented translation options: context and glossary are
        # template kwargs, not prompt text. Dictionary terms go in as
        # identity pairs so the model keeps them verbatim.
        if req.context:
            ctk["context"] = req.context
        if req.dictionary:
            ctk["glossary"] = [{"source": t, "target": t} for t in req.dictionary[:100]]
    completion_options = {"temperature": temperature, "chat_template_kwargs": ctk}
    if req.max_tokens is not None:
        completion_options["max_tokens"] = req.max_tokens
    try:
        async with registry._llm_semaphore:
            data = await llm.complete(
                messages,
                **completion_options,
            )
    except Exception as exc:  # noqa: BLE001 - surfaced as 502 with detail
        raise HTTPException(502, f"text processing failed: {exc}")

    choice = (data.get("choices") or [{}])[0]
    if req.max_tokens is None and choice.get("finish_reason") == "length":
        # A backend that did not finish must not pass a partial translation as
        # complete. Keep the original without imposing a service output cap.
        return JSONResponse(
            {
                "detail": (
                    "the text model returned an incomplete response; the original text was kept"
                ),
                "reason": "incomplete_generation",
                "original": req.text,
            },
            status_code=502,
        )
    content = choice.get("message", {}).get("content", "")
    result, notes = content, ""
    if req.explain and "---" in content:
        result, _, notes = content.partition("---")
        result, notes = result.strip(), notes.strip()
    # Models occasionally wrap output in quotes or mark identifiers with
    # backticks; dictated text goes straight into other applications, where
    # neither belongs.
    result = _strip_wrapping_quotes(result.replace("`", "").strip())
    if notes_bits:
        notes = (notes + "\n" if notes else "") + "; ".join(notes_bits)

    return JSONResponse(
        {
            "mode": req.mode,
            "profile": req.profile,
            "original": req.text,
            "result": result,
            **({"notes": notes} if notes else {}),
            "model": llm.model_alias,
            "duration_ms": int((time.perf_counter() - started) * 1000),
            "usage": data.get("usage"),
        }
    )


def _strip_wrapping_quotes(text: str) -> str:
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'«“”‘’":
        inner = text[1:-1].strip()
        return inner or text
    return text
