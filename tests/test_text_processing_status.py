"""/v1/text/process: success, incomplete generation and backend failure.

A result is only ever returned with 200 when the model produced it. When the
backend returns an incomplete response, the service answers 502 with a reason and
the original text, so no client can mistake the input for a correction or a
translation, and no client loses it.
"""
from __future__ import annotations

from pathlib import Path

import pytest
from fastapi.testclient import TestClient

from voiceservice.app import create_app


@pytest.fixture()
def service(tmp_path: Path, monkeypatch) -> TestClient:
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    (tmp_path / "runtime").mkdir()
    cfg = tmp_path / "config.yaml"
    cfg.write_text("llm:\n  skip_if_clean: false\nbackends:\n  llama:\n    template_style: ''\n  audiocpp_clone:\n    enabled: false\n  llama_translate:\n    enabled: false\n", encoding="utf-8")
    with TestClient(create_app(cfg), base_url="http://127.0.0.1") as client:
        yield client


def _model(monkeypatch, service, content: str, finish_reason: str = "stop", seen: dict | None = None):
    async def complete(messages, **kw):
        if seen is not None:
            seen["messages"], seen["kw"] = messages, kw
        return {"choices": [{"message": {"content": content}, "finish_reason": finish_reason}],
                "usage": {"completion_tokens": 5}}

    monkeypatch.setattr(service.app.state.registry.llm, "complete", complete)


def test_successful_correction_returns_the_model_result(service, monkeypatch) -> None:
    seen: dict = {}
    _model(monkeypatch, service, "Нам нужно исправить эту ошибку.", seen=seen)
    resp = service.post("/v1/text/process", json={"text": "нам нужно исправить эта ошибка", "mode": "correct_ru"})
    assert resp.status_code == 200
    body = resp.json()
    assert body["result"] == "Нам нужно исправить эту ошибку."
    assert body["original"] == "нам нужно исправить эта ошибка"
    assert set(body) >= {"mode", "profile", "original", "result", "model", "duration_ms", "usage"}
    assert "Russian text" in seen["messages"][0]["content"]


def test_successful_translation_returns_the_model_result(service, monkeypatch) -> None:
    seen: dict = {}
    _model(monkeypatch, service, "Open the file main.cpp.", seen=seen)
    resp = service.post("/v1/text/process", json={"text": "Открой файл main.cpp.", "mode": "translate_ru_en"})
    assert resp.status_code == 200
    assert resp.json()["result"] == "Open the file main.cpp."
    assert "Translate the Russian text into" in seen["messages"][0]["content"]


@pytest.mark.parametrize("mode,text", [
    ("translate_ru_en", "Прокачка персонажа должна зависеть от лута."),
    ("correct_en", "this are a test"),
])
def test_incomplete_generation_is_a_failure_that_returns_the_original(service, monkeypatch, mode, text) -> None:
    _model(monkeypatch, service, "loop loop loop", finish_reason="length")
    resp = service.post("/v1/text/process", json={"text": text, "mode": mode})
    assert resp.status_code == 502
    body = resp.json()
    assert body["reason"] == "incomplete_generation"
    assert body["original"] == text
    assert "max_tokens" not in body
    assert "result" not in body               # nothing a client could insert as processed
    assert "incomplete response" in body["detail"]


def test_backend_failure_is_reported_without_a_result(service, monkeypatch) -> None:
    async def complete(messages, **kw):
        raise RuntimeError("llama-server is not answering")

    monkeypatch.setattr(service.app.state.registry.llm, "complete", complete)
    resp = service.post("/v1/text/process", json={"text": "Keep this.", "mode": "translate_en_ru"})
    assert resp.status_code == 502
    assert "result" not in resp.json()
    assert "llama-server is not answering" in resp.json()["detail"]


def test_dictation_mode_never_calls_the_model(service, monkeypatch) -> None:
    async def complete(messages, **kw):
        raise AssertionError("dictation must not reach the LLM")

    monkeypatch.setattr(service.app.state.registry.llm, "complete", complete)
    resp = service.post("/v1/text/process", json={"text": " plain text ", "mode": "dictation"})
    assert resp.status_code == 200 and resp.json()["result"] == "plain text"


def test_long_correction_does_not_get_an_automatic_output_cap(service, monkeypatch) -> None:
    seen = {}
    _model(monkeypatch, service, "Исправленный текст.", seen=seen)
    russian = "Экономика игры строится на трёх валютах. " * 40
    response = service.post("/v1/text/process", json={"text": russian, "mode": "correct_ru"})
    assert response.status_code == 200
    assert "max_tokens" not in seen["kw"]
