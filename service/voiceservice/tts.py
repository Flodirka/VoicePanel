"""Speech request parameters supported by the configured engine and model."""
from __future__ import annotations

import math
from typing import Any

# ServeurpersoCom/omnivoice.cpp src/voice-design.h, also used by OmniVoice.
# The model accepts attribute keywords, not arbitrary prose.
VOICE_DESIGN = {
    "gender": ["male", "female"],
    "age": ["child", "teenager", "young adult", "middle-aged", "elderly"],
    "pitch": ["very low pitch", "low pitch", "moderate pitch", "high pitch", "very high pitch"],
    "style": ["whisper"],
    "accent": ["american accent", "british accent", "australian accent", "chinese accent",
               "canadian accent", "indian accent", "korean accent", "portuguese accent",
               "russian accent", "japanese accent"],
    "dialect": ["河南话", "陕西话", "四川话", "贵州话", "云南话", "桂林话",
                "济南话", "石家庄话", "甘肃话", "宁夏话", "青岛话", "东北话"],
}


def speech_parameters(engine: str, entry: dict[str, Any]) -> dict[str, Any]:
    family = entry.get("family", "")
    omni = family == "omnivoice"
    params = {
        "speed": engine != "omnivoice" and family in ("kokoro_tts", "piper_tts", "omnivoice"),
        "instructions": omni,
        "voice_cloning": omni or family in ("qwen3_tts", "chatterbox"),
        "voice_design": VOICE_DESIGN if omni else {},
        "options": (["steps", "seed"] if engine == "omnivoice" else
                    ["num_inference_steps", "guidance_scale", "seed"] if omni else
                    ["max_tokens"] if family == "qwen3_tts" else []),
    }
    # Explicit metadata permits a newly configured model to declare its options.
    params.update(entry.get("speech_parameters") or {})
    return params


def validate_instructions(value: str) -> str:
    items = [item.strip().lower() for item in value.split(",") if item.strip()]
    used: set[str] = set()
    for item in items:
        category = next((k for k, values in VOICE_DESIGN.items() if item in values), None)
        if category is None:
            raise ValueError(f"unsupported voice-design attribute '{item}'")
        if category in used:
            raise ValueError(f"choose one voice-design attribute for {category}")
        used.add(category)
    if "accent" in used and "dialect" in used:
        raise ValueError("voice design cannot combine an English accent and a Chinese dialect")
    return ", ".join(items)


def validate_speech_parameters(
    params: dict[str, Any], speed: Any, instructions: Any, options: dict[str, Any], voice: Any,
) -> tuple[float | None, dict[str, Any]]:
    if speed is not None:
        if isinstance(speed, bool) or not isinstance(speed, (float, int)) or not math.isfinite(speed) or speed <= 0:
            raise ValueError("speed must be a positive finite number")
        if not params["speed"]:
            if speed != 1:
                raise ValueError("the selected model does not support speed control")
            speed = None  # OpenAI clients commonly send the neutral default.
    options = dict(options)
    if instructions:
        if not params["instructions"]:
            raise ValueError("the selected model does not support voice design")
        if not isinstance(instructions, str):
            raise ValueError("instructions must be a string")
        if voice:
            raise ValueError("choose voice cloning or voice design for this request")
        options["instruct"] = validate_instructions(instructions)
    allowed = set(params["options"]) | ({"instruct"} if params["instructions"] else set())
    unknown = set(options) - allowed
    if unknown:
        raise ValueError(f"unsupported speech options: {', '.join(sorted(unknown))}")
    if options.get("instruct"):
        if not isinstance(options["instruct"], str):
            raise ValueError("instruct must be a string")
        if voice:
            raise ValueError("choose voice cloning or voice design for this request")
        options["instruct"] = validate_instructions(options["instruct"])
    for name in ("steps", "num_inference_steps", "seed"):
        if name in options:
            value = options[name]
            minimum = -1 if name == "seed" else 1
            maximum = 2147483647 if name == "seed" else 128
            if isinstance(value, bool) or not isinstance(value, int) or not minimum <= value <= maximum:
                raise ValueError(f"{name} must be an integer from {minimum} to {maximum}")
    if "guidance_scale" in options:
        value = options["guidance_scale"]
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value <= 0:
            raise ValueError("guidance_scale must be a positive finite number")
    return speed, options
