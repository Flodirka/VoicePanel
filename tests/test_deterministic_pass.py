"""Deterministic pre-pass (dictionary near-misses + spell-checker agreement)
and the skip-clean gate in /v1/text/process.

The pre-pass is model-free and runs in ~1 ms: it fixes what the ASR misheard
of the user's own terminology so the LLM only polishes what is left, and clean
sentences can skip the model entirely (llm.skip_if_clean).
"""
from __future__ import annotations

from voiceservice.processing import deterministic_pass


def test_no_dictionary_is_a_no_op() -> None:
    assert deterministic_pass("hello world") == ("hello world", [])


def test_near_miss_single_token_is_replaced() -> None:
    text, changes = deterministic_pass("open the drobpox folder", dictionary=["dropbox"])
    assert text == "open the dropbox folder"
    assert changes == [("drobpox", "dropbox")]


def test_glued_pair_is_joined() -> None:
    # The WinVosk case: the model split «дропбокс» into «друг бокс»; glued,
    # the pair scores exactly the 0.75 cutoff against the listed word.
    text, changes = deterministic_pass("друг бокс настроен", dictionary=["дропбокс"])
    assert text == "дропбокс настроен"
    assert changes == [("друг бокс", "дропбокс")]


def test_inflection_is_left_alone() -> None:
    # «дропбоксы» starts with the listed «дропбокс»: folding the inflection
    # back to the stem would be wrong.
    text, changes = deterministic_pass("в дропбоксах лежит", dictionary=["дропбокс"])
    assert text == "в дропбоксах лежит" and changes == []


def test_exact_term_is_never_touched() -> None:
    text, changes = deterministic_pass("дропбокс открыт", dictionary=["дропбокс"])
    assert text == "дропбокс открыт" and changes == []


def test_case_is_preserved() -> None:
    text, _ = deterministic_pass("Drobpox is open", dictionary=["dropbox"])
    assert text == "Dropbox is open"


def test_spell_hint_matching_dictionary_fixes_directly() -> None:
    text, changes = deterministic_pass(
        "helo drobpox",
        dictionary=["dropbox"],
        spelling=[{"word": "drobpox", "suggestions": ["dropbox"]}],
    )
    assert text == "helo dropbox"
    assert ("drobpox", "dropbox") in changes


def test_checker_proposed_replacement_is_applied_verbatim() -> None:
    # CORRECTIVE_ACTION_REPLACE from the Windows checker: agreement fix
    # applied without a model, even with no dictionary at all.
    text, changes = deterministic_pass(
        "мы вчера пошли и купил молоко",
        spelling=[{"word": "купил", "suggestions": ["купили"], "proposed": "купили"}],
    )
    assert text == "мы вчера пошли и купили молоко"
    assert ("купил", "купили") in changes


def test_proposed_without_match_and_no_dictionary_changes_nothing() -> None:
    text, changes = deterministic_pass("hello world", spelling=[{"word": "hello", "suggestions": ["hi"]}])
    assert text == "hello world" and changes == []


def test_spell_hint_without_dictionary_match_is_left_for_the_model() -> None:
    text, changes = deterministic_pass(
        "helo world", dictionary=["dropbox"], spelling=[{"word": "helo", "suggestions": ["hello"]}]
    )
    assert text == "helo world" and changes == []


# ------------------------------------------------------------- API behaviour
from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from voiceservice.app import create_app


@pytest.fixture()
def service(tmp_path: Path, monkeypatch) -> TestClient:
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    (tmp_path / "runtime").mkdir()
    cfg = tmp_path / "config.yaml"
    cfg.write_text("backends:\n  audiocpp_clone:\n    enabled: false\n  llama_translate:\n    enabled: false\n", encoding="utf-8")
    with TestClient(create_app(cfg), base_url="http://127.0.0.1") as client:
        yield client


def _fail_if_called(monkeypatch, service) -> None:
    async def complete(messages, **kw):
        raise AssertionError("the model must not be called")

    monkeypatch.setattr(service.app.state.registry.llm, "complete", complete)


def test_clean_correction_skips_the_model_by_default(service, monkeypatch) -> None:
    _fail_if_called(monkeypatch, service)
    resp = service.post("/v1/text/process", json={"text": "This is already correct.", "mode": "correct_en"})
    assert resp.status_code == 200
    body = resp.json()
    assert body["result"] == "This is already correct."
    assert body["model"] is None
    assert body["notes"] == "clean"


def test_spelling_hints_still_reach_the_model(service, monkeypatch) -> None:
    seen: dict = {}

    async def complete(messages, **kw):
        seen["messages"] = messages
        return {"choices": [{"message": {"content": "This is a test."}, "finish_reason": "stop"}]}

    monkeypatch.setattr(service.app.state.registry.llm, "complete", complete)
    resp = service.post(
        "/v1/text/process",
        json={"text": "this are a test", "mode": "correct_en",
              "spelling": [{"word": "are", "suggestions": ["is"]}]},
    )
    assert resp.status_code == 200 and resp.json()["result"] == "This is a test."
    assert "spell checker flagged" in seen["messages"][0]["content"]


def test_dictionary_fix_is_applied_before_the_model_and_reported(service, monkeypatch) -> None:
    seen: dict = {}

    async def complete(messages, **kw):
        seen["messages"] = messages
        return {"choices": [{"message": {"content": messages[-1]["content"]}, "finish_reason": "stop"}]}

    monkeypatch.setattr(service.app.state.registry.llm, "complete", complete)
    resp = service.post(
        "/v1/text/process",
        json={"text": "put it in drobpox please", "mode": "correct_en", "dictionary": ["dropbox"]},
    )
    assert resp.status_code == 200
    # The model saw the fixed text, and the change is reported in notes.
    assert seen["messages"][-1]["content"] == "put it in dropbox please"
    assert "drobpox -> dropbox" in resp.json()["notes"]


def test_skip_if_clean_can_be_disabled(tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    (tmp_path / "runtime").mkdir(exist_ok=True)
    cfg = tmp_path / "config2.yaml"
    cfg.write_text(
        "llm:\n  skip_if_clean: false\nbackends:\n  audiocpp_clone:\n    enabled: false\n  llama_translate:\n    enabled: false\n",
        encoding="utf-8",
    )
    with TestClient(create_app(cfg), base_url="http://127.0.0.1") as client:
        async def complete(messages, **kw):
            return {"choices": [{"message": {"content": "Fixed."}, "finish_reason": "stop"}]}

        monkeypatch.setattr(client.app.state.registry.llm, "complete", complete)
        resp = client.post("/v1/text/process", json={"text": "This is already correct.", "mode": "correct_en"})
        assert resp.status_code == 200 and resp.json()["result"] == "Fixed."


def test_translation_modes_route_to_the_dedicated_translator(service, monkeypatch) -> None:
    registry = service.app.state.registry
    calls: list[str] = []

    async def main_complete(messages, **kw):
        calls.append("main")
        return {"choices": [{"message": {"content": "x"}, "finish_reason": "stop"}]}

    async def tr_complete(messages, **kw):
        calls.append("translate")
        return {"choices": [{"message": {"content": "x"}, "finish_reason": "stop"}]}

    monkeypatch.setattr(registry.llm, "complete", main_complete)

    class FakeTranslate:
        external = False
        model_alias = "hy-mt2-1.8b"
        cfg = {}
        complete = staticmethod(tr_complete)

    registry.llm_translate = FakeTranslate()
    resp = service.post("/v1/text/process", json={"text": "привет", "mode": "translate_ru_en"})
    assert resp.status_code == 200
    assert calls == ["translate"]
    assert resp.json()["model"] == "hy-mt2-1.8b"
    # Correction still uses the main model.
    calls.clear()
    resp = service.post("/v1/text/process", json={"text": "helo there", "mode": "correct_en",
                                                  "spelling": [{"word": "helo", "suggestions": ["hello"]}]})
    assert calls == ["main"]
    text, changes = deterministic_pass(
        "helo world", dictionary=["dropbox"], spelling=[{"word": "helo", "suggestions": ["hello"]}]
    )
    assert text == "helo world" and changes == []


def test_short_tokens_are_ignored() -> None:
    text, changes = deterministic_pass("the cat sat", dictionary=["cat"])
    assert text == "the cat sat" and changes == []


def test_multiword_dictionary_entries_are_ignored() -> None:
    # A phrase cannot match one token; only single words are glued/compared.
    text, changes = deterministic_pass("visual studio code", dictionary=["visual studio code"])
    assert text == "visual studio code" and changes == []