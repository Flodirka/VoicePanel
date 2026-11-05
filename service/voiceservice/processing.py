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
        "Fix only what is objectively wrong in the English text: grammar, agreement, "
        "missing or wrong punctuation, capitalisation, and words the speaker clearly "
        "did not say. Do not add information and do not remove content. Make the "
        "smallest edit that fixes the fault: do not rephrase, do not improve style, "
        "do not replace a word that is already correct with a synonym, do not merge or "
        "split sentences, do not drop the speaker's fillers or repetitions. If the text "
        "is already correct, return it unchanged; a grammatical sentence must come back "
        "byte-for-byte identical. When in doubt, change nothing."
    ),
    "correct_ru": (
        "Fix only what is objectively wrong in the Russian text: grammar, agreement, "
        "missing or wrong punctuation, capitalisation, and words the speaker clearly "
        "did not say. Do not add information and do not remove content. Make the "
        "smallest edit that fixes the fault: do not rephrase, do not improve style, "
        "do not replace a word that is already correct with a synonym, do not merge or "
        "split sentences, do not drop the speaker's fillers or repetitions. If the text "
        "is already correct, return it unchanged; a grammatical sentence must come back "
        "byte-for-byte identical. When in doubt, change nothing."
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
        proposed = str(hint.get("proposed", "")).strip()
        if proposed and proposed not in suggestions:
            suggestions.insert(0, proposed)
        if proposed:
            lines.append(f'- "{word}" -> {proposed} (the spell checker itself proposes this replacement)')
        elif suggestions:
            lines.append(f'- "{word}" -> {", ".join(suggestions)}')
        else:
            lines.append(f'- "{word}"')
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
    #
    # Order matters for speed: llama-server reuses the KV cache of the longest
    # prefix shared with the previous request. Parts that stay the same for a
    # whole session (rules, profile, terminology, context document) come first;
    # parts that change per request (the task, which follows the language of
    # each sentence in auto mode, and spell-checker hints) come last. Then a
    # switch between correct_ru and correct_en re-evaluates only the task line,
    # not the whole context document.
    parts = [SYSTEM_BASE, ""]
    profile_text = PROFILES.get(profile)
    if profile_text:
        parts.append(f"Context domain: {profile_text}")
    if dictionary:
        parts.append("User terminology (prefer these renderings): " + "; ".join(dictionary[:200]))
    if context:
        parts.append(context)
    parts += ["", f"Task: {task}"]
    if spelling:
        section = _spelling_section(spelling)
        if section:
            parts.append(section)
    system = "\n".join(parts)
    return [
        {"role": "system", "content": system},
        {"role": "user", "content": text},
    ]


# --------------------------------------------------------------------------
# Whisper/Qwen transcripts sometimes contain bracketed non-speech annotations
# ("[BLANK_AUDIO]", "(silence)", "[Music]"); a transcript made only of them is
# empty. Mirrors Pithagoras' cleanTranscript.
# Some models emit a language tag inside the text ("<mt-MT>" when Nemotron
# guesses Maltese for Russian speech, "<|en|>" from template-style models).
# It is metadata, never a word the speaker said.
_LANG_TAG = re.compile(r"<\|?[a-z]{2,3}[-_][A-Z]{2}\|?>|<\|[a-z_]+\|>")

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
    # Removing a tag leaves a double space where it sat.
    stripped = re.sub(r"[ \t]{2,}", " ", _LANG_TAG.sub("", text)).strip()
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


# --------------------------------------------------------------------------
# Deterministic pre-pass: dictionary near-misses and spell-checker hits that
# match the user's terminology are fixed without the model. Inspired by
# WinVosk's corrector.py. Runs on the finished text only, in ~1 ms per
# sentence, and reports every replacement so the caller can show or log it.
#
# Measured cutoffs (WinVosk, 138 real words from logs): a dropped letter
# scores about 0.93, a wrong consonant about 0.80, so 0.75 catches both with
# a ~0.03 % false-replacement rate against random tokens.
DICT_NEAR_MISS_CUTOFF = 0.75
DICT_MIN_LENGTH = 4        # shorter words are where wrong fixes are invisible
JOIN_MIN = 3               # min length of each half of a glued pair


def _norm_word(word: str) -> str:
    """Case- and ё/е-insensitive comparison key."""
    return word.strip().lower().replace("ё", "е")


def _ratio(a: str, b: str) -> float:
    from difflib import SequenceMatcher

    return SequenceMatcher(None, a, b).ratio()


def _closest(word: str, words: list[str], cutoff: float = DICT_NEAR_MISS_CUTOFF) -> str | None:
    """The listed word `word` should become, or None to leave it alone.

    An exact hit and an inflection («дропбоксы» for «дропбокс») are left
    alone: Russian inflects heavily, so folding a form back to its stem would
    be wrong more often than right. Add the forms you actually say.
    """
    best, best_ratio = None, 0.0
    for candidate in words:
        r = _ratio(word, candidate)
        if r > best_ratio:
            best, best_ratio = candidate, r
    if best is None or best_ratio < cutoff:
        return None
    if best == word or word.startswith(best):
        return None
    return best


def _match_case(original: str, replacement: str) -> str:
    """Keep the shape of the heard token: capitalised stays capitalised."""
    if original[:1].isupper():
        return replacement[:1].upper() + replacement[1:]
    return replacement


def deterministic_pass(
    text: str,
    dictionary: list[str] | None = None,
    spelling: list[dict[str, Any]] | None = None,
) -> tuple[str, list[tuple[str, str]]]:
    """Fix transcript tokens that are near-misses of known user terms.

    Returns (possibly fixed text, [(heard, replaced), ...]). Two sources:

    * the user dictionary: a token (or two glued tokens, for splits like
      «друг бокс» -> «дропбокс») within the cutoff of a listed word;
    * spell-checker hints: a flagged word with the checker's own REPLACE
      proposal (grammar-style fixes) is replaced by that proposal outright,
      and a flagged word that is a near-miss of a listed word is replaced by
      the listed word — the checker and the dictionary agree, so no model
      needs to decide.

    Hints that match no dictionary word are left for the LLM, exactly as
    before. Tokens that already are dictionary words are never touched.
    """
    if not text or not text.strip():
        return text, []
    terms = [
        _norm_word(w)
        for w in (dictionary or [])
        if " " not in w.strip() and len(_norm_word(w)) >= DICT_MIN_LENGTH
    ]
    terms = list(dict.fromkeys(terms))
    spell_fix: dict[str, str] = {}
    if spelling:
        for hint in spelling:
            word = _norm_word(str(hint.get("word", "")))
            if len(word) < 2:
                continue
            # The checker's own REPLACE correction (grammar-style fixes such as
            # agreement, also short words like "are" -> "is") is applied
            # verbatim: it is a high-confidence signal, not a guess. A near-miss
            # of a dictionary word is fixed to the listed word when they agree.
            proposed = str(hint.get("proposed", "")).strip()
            if proposed and len(_norm_word(proposed)) >= 2:
                spell_fix[word] = _norm_word(proposed)
                continue
            if len(word) >= DICT_MIN_LENGTH and terms:
                hit = _closest(word, terms)
                if hit and hit != word:
                    spell_fix[word] = hit
    if not terms and not spell_fix:
        return text, []

    tokens = text.split()
    out: list[str] = []
    changes: list[tuple[str, str]] = []
    i = 0
    while i < len(tokens):
        token = tokens[i]
        following = tokens[i + 1] if i + 1 < len(tokens) else ""
        key = _norm_word(token)
        replacement: str | None = None
        consumed = 0
        if key in spell_fix:
            replacement = spell_fix[key]
            changes.append((token, replacement))
        elif len(key) >= DICT_MIN_LENGTH and key not in terms:
            nxt = _norm_word(following)
            if len(key) >= JOIN_MIN and len(nxt) >= JOIN_MIN:
                glued = _closest(key + nxt, terms)
                if glued:
                    replacement = glued
                    consumed = 1
                    changes.append((f"{token} {following}", glued))
            if replacement is None:
                single = _closest(key, terms)
                if single:
                    replacement = single
                    changes.append((token, single))
        out.append(_match_case(token, replacement) if replacement else token)
        i += 1 + consumed
    if not changes:
        return text, changes
    return " ".join(out), changes
