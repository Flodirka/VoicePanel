from voiceservice.processing import build_messages


def test_personal_context_is_not_silently_truncated():
    context = "Game development vocabulary. " * 100 + "CustomTermBeyondTwoThousand"
    messages = build_messages("A complete sentence.", "correct_en", context=context)
    assert context in messages[0]["content"]


def test_correction_and_translation_prompts_disallow_em_dashes_in_prose():
    for mode in ("correct_ru", "correct_en", "translate_ru_en", "translate_en_ru"):
        system = build_messages("A complete sentence.", mode)[0]["content"]
        assert "Do not use em dashes (U+2014) in prose" in system
        assert "code" in system
