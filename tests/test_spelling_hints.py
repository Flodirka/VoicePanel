"""Spell-checker hints in the correction prompt."""
from __future__ import annotations

from voiceservice.processing import build_messages


def _system(text: str, mode: str = "correct_en", spelling=None) -> str:
    return build_messages(text, mode=mode, spelling=spelling)[0]["content"]


def test_hints_reach_the_prompt_and_stay_suggestions() -> None:
    system = _system("this are a test", spelling=[{"word": "teh", "suggestions": ["the", "tea"]}])
    assert "A spell checker flagged these words" in system
    assert '- "teh" -> the, tea' in system
    assert "never change names, paths, commands, versions or identifiers" in system


def test_no_hints_no_section() -> None:
    assert "spell checker flagged" not in _system("this is fine")


def test_empty_hints_are_ignored() -> None:
    assert "spell checker flagged" not in _system("this is fine", spelling=[])


def test_correction_rules_stay_off_for_the_four_b_model() -> None:
    # They were added for Qwen3.5-0.8B. With the 4B, the English rules and the
    # English example pushed Russian correction into English output.
    system = _system("this are a test of teh model")
    assert "verb agreement" not in system
    assert "Example:" not in system


def test_translation_is_not_a_correction_prompt() -> None:
    system = _system("привет", mode="translate_ru_en")
    assert "Example:" not in system
    assert "verb agreement" not in system
