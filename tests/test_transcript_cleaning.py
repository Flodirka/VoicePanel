"""Transcript cleaning: CJK artifacts from the recogniser."""
from __future__ import annotations

import pytest

from voiceservice.processing import clean_transcript


def test_cjk_artifact_inside_russian_text_is_dropped() -> None:
    text = "Долго, долго печатать, долго печатать много. 嗯。 Поэтому ты долго этим не занимался."
    assert clean_transcript(text) == (
        "Долго, долго печатать, долго печатать много. Поэтому ты долго этим не занимался."
    )


def test_artifact_leaves_no_stray_space_before_punctuation() -> None:
    assert clean_transcript("Так, тестируем 嗯, как это будет работать.") == (
        "Так, тестируем, как это будет работать."
    )


def test_a_segment_that_is_only_the_artifact_is_empty() -> None:
    assert clean_transcript("嗯。") == ""
    assert clean_transcript("嗯") == ""


def test_genuine_foreign_text_is_left_alone() -> None:
    text = "你好，世界。这是一个测试。"
    assert clean_transcript(text) == text


def test_clean_russian_and_english_are_untouched() -> None:
    for text in ("Привет. Это проверка.", "Hello, this is a test.", "API, commit, pipeline."):
        assert clean_transcript(text) == text


@pytest.mark.parametrize("text", ["hello 嗯 world 啊 test", "Мы закончили. 好的。"])
def test_mixed_script_keeps_the_spoken_language(text: str) -> None:
    assert clean_transcript(text) in ("hello world test", "Мы закончили.")
