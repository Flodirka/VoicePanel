"""Structure of the correction and translation prompts.

The session-stable parts (rules, profile, terminology, context document) come
first so llama.cpp can reuse their KV cache; the per-request task follows, then
optional spell-checker hints. These tests pin that layout and the content each
mode needs. They do not show that model quality is unchanged: that needs the
local A/B in docs/performance-audit.md.
"""
from __future__ import annotations

import pytest

from voiceservice.api.warmup import _LLM_MODES
from voiceservice.processing import (
    IDENTIFIER_RULES,
    MODE_PROMPTS,
    PROFILES,
    SYSTEM_BASE,
    build_messages,
)

CONTEXT = "Glossary: прокачка = progression; лут = loot. Keep UCLASS and GDScript as written."
MODES = ("correct_en", "correct_ru", "translate_ru_en", "translate_en_ru")


@pytest.mark.parametrize("mode", MODES)
def test_layout_is_rules_then_session_parts_then_task(mode: str) -> None:
    messages = build_messages("Текст.", mode, profile="unreal", dictionary=["UCLASS", "Blueprint"], context=CONTEXT)
    assert [m["role"] for m in messages] == ["system", "user"]
    assert messages[1]["content"] == "Текст."                      # the user turn is the text only
    system = messages[0]["content"]
    assert system.startswith(SYSTEM_BASE)
    assert IDENTIFIER_RULES in system
    profile = system.index(PROFILES["unreal"])
    terms = system.index("User terminology (prefer these renderings): UCLASS; Blueprint")
    context = system.index(CONTEXT)
    task = system.index("Task: " + MODE_PROMPTS[mode])
    assert len(SYSTEM_BASE) < profile < terms < context < task
    assert system.count("Task: ") == 1 and system.count(CONTEXT) == 1
    assert system.rstrip().endswith(MODE_PROMPTS[mode])            # the instruction is closest to the text


@pytest.mark.parametrize("mode,language", [("correct_en", "English"), ("correct_ru", "Russian")])
def test_correction_names_its_language_and_forbids_additions(mode: str, language: str) -> None:
    task = build_messages("x", mode)[0]["content"].split("Task: ", 1)[1]
    assert f"{language} text" in task
    assert "Do not add information" in task
    assert "return it unchanged" in task
    assert "Translate" not in task


@pytest.mark.parametrize("mode,source,target", [("translate_ru_en", "Russian", "English"),
                                                ("translate_en_ru", "English", "Russian")])
def test_translation_names_both_languages(mode: str, source: str, target: str) -> None:
    task = build_messages("x", mode)[0]["content"].split("Task: ", 1)[1]
    assert task.startswith(f"Translate the {source} text into natural, precise {target}")
    assert "Correct grammar" not in task


def test_spelling_hints_follow_the_task() -> None:
    system = build_messages("teh text", "correct_en", context=CONTEXT,
                            spelling=[{"word": "teh", "suggestions": ["the"]}])[0]["content"]
    assert system.index("Task: ") < system.index("A spell checker flagged these words")
    assert '- "teh" -> the' in system


@pytest.mark.parametrize("profile", sorted(PROFILES))
def test_every_profile_reaches_the_prompt(profile: str) -> None:
    assert PROFILES[profile] in build_messages("x", "correct_en", profile=profile)[0]["content"]


def test_unknown_profile_and_missing_context_are_omitted() -> None:
    system = build_messages("x", "correct_en", profile="does-not-exist")[0]["content"]
    assert "Context domain:" not in system
    assert system.rstrip().endswith(MODE_PROMPTS["correct_en"])


def test_unknown_mode_is_rejected() -> None:
    with pytest.raises(ValueError):
        build_messages("x", "dictation")


def test_session_prefix_is_identical_across_modes() -> None:
    """Everything before the task line is byte-identical for all four modes,
    so a language or mode switch can reuse it (llama.cpp cache_prompt)."""
    prefixes = {build_messages("x", m, context=CONTEXT)[0]["content"].split("Task: ", 1)[0] for m in MODES}
    assert len(prefixes) == 1
    assert CONTEXT in prefixes.pop()


def test_warmup_modes_map_to_real_modes() -> None:
    assert set(_LLM_MODES.values()) <= set(MODE_PROMPTS)
    assert _LLM_MODES["auto_correct"].startswith("correct_")
    assert _LLM_MODES["auto_translate"].startswith("translate_")
