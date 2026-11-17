from pathlib import Path

from fastapi.testclient import TestClient

from voiceservice.app import create_app


def test_correction_uses_the_trained_prompt_without_an_automatic_output_cap(tmp_path: Path, monkeypatch):
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "")
    config = tmp_path / "config.yaml"
    config.write_text("llm:\n  skip_if_clean: false\nbackends:\n  llama:\n    template_style: loqira_gec\n", encoding="utf-8")
    with TestClient(create_app(config), base_url="http://127.0.0.1") as client:
        seen = {}

        async def complete(messages, **kwargs):
            seen.update(messages=messages, kwargs=kwargs)
            return {"choices": [{"message": {"content": "Первое предложение."}, "finish_reason": "stop"}]}

        monkeypatch.setattr(client.app.state.registry.llm, "complete", complete)
        response = client.post("/v1/text/process", json={
            "text": "первое предложение", "mode": "correct_ru", "previous": "Предыдущая фраза.",
            "context": "Unrelated document instructions.",
        })
        assert response.status_code == 200
        assert seen["messages"] == [
            {"role": "system", "content": "Отформатируй текст голосового ввода: расставь пунктуацию и заглавные буквы, "
             "разбей на абзацы, исправь опечатки и орфографические ошибки. Сохрани язык, слова и смысл, ничего не добавляй от себя."},
            {"role": "user", "content": "первое предложение"},
        ]
        assert "max_tokens" not in seen["kwargs"]
        assert seen["kwargs"]["chat_template_kwargs"]["enable_thinking"] is False
