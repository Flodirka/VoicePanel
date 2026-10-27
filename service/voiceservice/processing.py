"""Text processing modes and context profiles for translation and correction.

Design constraints from the specification:

* Original meaning and technical identifiers must survive: class names, API
  names, file paths, variables, CLI commands, code fragments, versions,
  abbreviations.
* Modes: plain dictation (no LLM), RU->EN (for prompts/instructions),
  EN correction, EN->RU (for understanding responses).
* Profiles tune terminology; user dictionaries inject extra terms.
"""
from __future__ import annotations

import re

MODES = ("dictation", "translate_ru_en", "translate_en_ru", "correct_en", "correct_ru")

PROFILES: dict[str, str] = {
    "general": "General software and everyday technical conversation.",
    "ai_prompts": "Prompts and instructions for AI assistants and LLM agents. Keep prompt structure, role/task/format sections, and placeholder names intact.",
    "programming": "Source code discussion. Preserve identifiers, symbols, operators, and formatting exactly; do not translate code.",
    "godot": "Godot game engine: GDScript, nodes, scenes, signals, exports (Node2D, _ready, get_node, tscn, gdextension).",
    "unity": "Unity: C# scripting, GameObjects, components, prefabs, ScriptableObjects, the Unity Editor UI.",
    "unreal": "Unreal Engine: Blueprints, UCLASS/UFUNCTION markup, C++ gameplay framework, assets (uasset, umap).",
}

IDENTIFIER_RULES = """
Preserve exactly, never translating, transliterating, or reformatting:
- class, function, variable and constant names (loadUserConfig, MAX_RETRIES)
- API and command names (GET /v1/audio, npm run build --release, git rebase -i)
- file and directory paths (src/config/service.py, C:\\Users\\name)
- code fragments and anything inside backticks or quotes given as code
- version numbers, flags, and technical abbreviations (v2.4.1, --force, VRAM, API)
""".strip()

SYSTEM_BASE = (
    "You are a precise multilingual technical text processor. You output only the "
    "processed text with no preamble, explanations, or quotes around the result.\n"
    "Do not use em dashes (U+2014) in prose. Rephrase with commas, colons, or "
    "separate sentences. Preserve protected code and identifiers exactly.\n"
    + IDENTIFIER_RULES
)

MODE_PROMPTS: dict[str, str] = {
    "translate_ru_en": (
        "Translate the Russian text into natural, precise English as a fluent "
        "technical writer would. Prefer terminology standard in software "
        "documentation. Keep sentence order logical; never add or drop content. "
        "Keep the output format (line breaks, lists) of the source."
    ),
    "translate_en_ru": (
        "Translate the English text into natural, precise Russian as an experienced "
        "technical translator would. Use accepted Russian IT terminology; keep "
        "English terms that are customary in Russian technical speech (API, commit, "
        "pipeline) in Latin script. Keep the output format of the source."
    ),
    "correct_en": (
        "Correct grammar, word choice, punctuation, and natural expression of the "
        "English text. Keep the author's meaning, tone, and formatting. Do not add "
        "information. If the text is already correct, return it unchanged."
    ),
    "correct_ru": (
        "Correct grammar, word choice, punctuation, and natural expression of the "
        "Russian text. Keep the author's meaning, tone, and formatting. Do not add "
        "information. If the text is already correct, return it unchanged."
    ),
}

# A small model misses agreement and capitalization unless it is told exactly
# what "correct" covers, and one example of the transformation carries more than
# another sentence of instructions. Measured on Qwen3.5-0.8B: without the example
# "this are a test of teh model" came back with "are" untouched.
#
# They are OFF for the 4B: an English rules block and an English example pushed
# a Russian correction task into English output ("нам надо поправить..." came
# back as "We need to fix..."). Only switch them on together with the small model.
USE_CORRECTION_RULES = False

CORRECTION_RULES = (
    "Fix grammar completely: verb agreement, articles, tense, capitalization, "
    "punctuation and prepositions, not only spelling. Keep the author's words "
    "where they are already correct. Output plain text only: no Markdown, no "
    "backticks, no added quotes, no explanations."
)
CORRECTION_EXAMPLE = 'Example: "this are a test of teh model" -> "This is a test of the model."'
CORRECTION_MODES = ("correct_en", "correct_ru")


def _spelling_section(spelling: list[dict[str, Any]]) -> str | None:
    """Spell-checker hits as a hint list; the model decides, nothing is replaced."""
    lines: list[str] = []
    for hint in spelling[:12]:
        word = str(hint.get("word", "")).strip()
        if not word:
            continue
        suggestions = [str(s) for s in (hint.get("suggestions") or []) if str(s).strip()][:3]
        lines.append(f'- "{word}" -> {", ".join(suggestions)}' if suggestions else f'- "{word}"')
    if not lines:
        return None
    return (
        "A spell checker flagged these words. Use a suggestion only when it fits the "
        "sentence; never change names, paths, commands, versions or identifiers:\n"
        + "\n".join(lines)
    )


def build_messages(
    text: str,
    mode: str,
    profile: str = "general",
    dictionary: list[str] | None = None,
    context: str | None = None,
    previous: str | None = None,
    spelling: list[dict[str, Any]] | None = None,
) -> list[dict[str, str]]:
    if mode not in MODE_PROMPTS:
        raise ValueError(f"unsupported mode '{mode}'")
    task = MODE_PROMPTS[mode]
    if USE_CORRECTION_RULES and mode in CORRECTION_MODES:
        task = f"{task} {CORRECTION_RULES}\n{CORRECTION_EXAMPLE}"
    # The system message carries everything the model needs: the rules, the task
    # and the context document, as the user asked. The user message is the text
    # and nothing else. `previous` is gone: feeding the model its own last output
    # made it repeat whole paragraphs.
    parts = [SYSTEM_BASE, "", f"Task: {task}", ""]
    profile_text = PROFILES.get(profile)
    if profile_text:
        parts.append(f"Context domain: {profile_text}")
    if dictionary:
        parts.append("User terminology (prefer these renderings): " + "; ".join(dictionary[:200]))
    if spelling:
        section = _spelling_section(spelling)
        if section:
            parts.append(section)
    if context:
        parts.append(context)
    system = "\n".join(parts)
    return [
        {"role": "system", "content": system},
        {"role": "user", "content": text},
    ]


# --------------------------------------------------------------------------
# Whisper/Qwen transcripts sometimes contain bracketed non-speech annotations
# ("[BLANK_AUDIO]", "(silence)", "[Music]"); a transcript made only of them is
# empty. Mirrors Pithagoras' cleanTranscript.
_BRACKET_ONLY = re.compile(r"^(?:\s*[[（(\[][^\])]*[\])）]])+\s*$")


# Qwen3-ASR answers an unclear stretch with CJK text ("嗯。") inside otherwise
# Russian or English speech: it is a Chinese-first model and falls back to its
# strongest language when the audio is noisy or out of domain. The product
# recognises Russian and English, so those characters are dropped before the
# text reaches the preview or the correcting model. Text that is mostly CJK is
# left alone - that is a different input, not an artifact.
_FOREIGN_SCRIPT = re.compile(
    "[\u3000-\u303f\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uac00-\ud7af\uff01-\uff60]"
)


def strip_foreign_scripts(text: str) -> str:
    if not _FOREIGN_SCRIPT.search(text):
        return text
    letters = [ch for ch in text if ch.isalpha()]
    if not letters:
        return text
    foreign = sum(1 for ch in letters if _FOREIGN_SCRIPT.match(ch))
    if foreign == len(letters) and len(letters) <= 3:
        return ""      # a one-word artifact, not a sentence
    if foreign / len(letters) > 0.5:
        return text
    cleaned = _FOREIGN_SCRIPT.sub("", text)
    cleaned = re.sub(r"\s+([,.!?;:])", r"\1", cleaned)
    cleaned = re.sub(r"[ \t]{2,}", " ", cleaned).strip()
    if not re.search(r"[A-Za-z\u0400-\u04FF]", cleaned):
        return ""      # the segment was nothing but the artifact
    return cleaned


def clean_transcript(text: str) -> str:
    stripped = text.strip()
    if _BRACKET_ONLY.match(stripped):
        return ""
    return strip_foreign_scripts(stripped)


# --------------------------------------------------------------------------
# Sentence-aware processing helpers. Correction must never see unstable
# fragments; the minimum unit is one complete sentence. These helpers find
# sentence boundaries in a stable transcript tail.

_SENTENCE_END = re.compile(r"(?<=[.!?…])\s+|\n+")


def split_sentences(text: str) -> list[str]:
    """Split into sentences; keeps punctuation. Empty segments dropped."""
    parts = [p.strip() for p in _SENTENCE_END.split(text)]
    return [p for p in parts if p]


# Sentence-final punctuation not embedded in numbers/versions ("2.4.1").
_FINAL_PUNCT = re.compile(r"(?<!\d)[.!?…]+(?:\s+|$)")


def complete_sentence_prefix(text: str) -> tuple[str, str]:
    """Split text into (complete sentences, trailing incomplete fragment).

    The fragment is the text after the last sentence-final punctuation; if the
    text ends with punctuation the fragment is empty.
    """
    stripped = text.strip()
    matches = list(_FINAL_PUNCT.finditer(stripped))
    if not matches:
        return "", stripped
    last = matches[-1]
    complete = stripped[: last.end()].strip()
    fragment = stripped[last.end():].strip()
    return complete, fragment
