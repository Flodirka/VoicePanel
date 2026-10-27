"""Network boundary checks; all inputs and credentials are synthetic."""
from __future__ import annotations

import pytest
from fastapi import Request, WebSocket
from fastapi.testclient import TestClient
from starlette.websockets import WebSocketDisconnect

from voiceservice.app import create_app


def make_app(tmp_path, monkeypatch, *, token=""):
    monkeypatch.delenv("VOICE_SERVICE_AUTH_TOKEN", raising=False)
    (tmp_path / "runtime").mkdir(exist_ok=True)
    config = tmp_path / "config.yaml"
    config.write_text(
        f"service:\n  auth_token: '{token}'\n  max_upload_mb: 1\n", encoding="utf-8"
    )
    app = create_app(config)

    @app.post("/test-body")
    async def body(request: Request):
        return {"size": len(await request.body())}

    @app.websocket("/test-websocket")
    async def websocket(ws: WebSocket):
        await ws.accept()
        await ws.send_text("ready")
        await ws.receive_bytes()
        await ws.send_text("received")

    return app


def test_http_rejects_rebinding_host_and_browser_origin(tmp_path, monkeypatch):
    app = make_app(tmp_path, monkeypatch)
    with TestClient(app, base_url="http://127.0.0.1") as client:
        assert client.post("/test-body", content=b"ok").status_code == 200
        assert client.post("/test-body", headers={"Host": "attacker.invalid"}).status_code == 400
        assert client.post("/test-body", headers={"Origin": "https://attacker.invalid"}).status_code == 403
        assert client.post("/test-body", headers={"Origin": "null"}).status_code == 403


@pytest.mark.parametrize("headers", [{"origin": "https://attacker.invalid"}, {"host": "attacker.invalid"}])
def test_websocket_rejects_browser_and_rebinding_attacks(tmp_path, monkeypatch, headers):
    app = make_app(tmp_path, monkeypatch)
    with TestClient(app, base_url="http://127.0.0.1") as client:
        with pytest.raises(WebSocketDisconnect) as closed:
            with client.websocket_connect("ws://127.0.0.1/v1/sessions/dictation", headers=headers):
                pass
        assert closed.value.code == 1008


def test_websocket_requires_the_same_token_as_http(tmp_path, monkeypatch):
    app = make_app(tmp_path, monkeypatch, token="synthetic-test-token")
    with TestClient(app, base_url="http://127.0.0.1") as client:
        assert client.post("/test-body").status_code == 401
        assert client.post("/test-body", headers={"X-API-Key": "synthetic-test-token"}).status_code == 200
        with pytest.raises(WebSocketDisconnect) as closed:
            with client.websocket_connect("ws://127.0.0.1/v1/sessions/dictation"):
                pass
        assert closed.value.code == 1008
        with client.websocket_connect("ws://127.0.0.1/test-websocket", headers={"Authorization": "Bearer synthetic-test-token"}) as ws:
            assert ws.receive_text() == "ready"
            ws.send_bytes(b"ok")
            assert ws.receive_text() == "received"


def test_authentication_environment_variable_is_applied(tmp_path, monkeypatch):
    app = make_app(tmp_path, monkeypatch)
    monkeypatch.setenv("VOICE_SERVICE_AUTH_TOKEN", "synthetic-environment-token")
    with TestClient(app, base_url="http://127.0.0.1") as client:
        assert client.post("/test-body").status_code == 401
        assert client.post("/test-body", headers={"Authorization": "Bearer synthetic-environment-token"}).status_code == 200


def test_oversized_body_is_rejected_even_without_content_length(tmp_path, monkeypatch):
    app = make_app(tmp_path, monkeypatch)
    with TestClient(app, base_url="http://127.0.0.1") as client:
        assert client.post("/test-body", content=b"x" * (1024 * 1024 + 1)).status_code == 413
        response = client.post("/test-body", content=iter([b"x" * (512 * 1024)] * 3))
        assert response.status_code == 413
        assert client.post("/test-body", headers={"Content-Length": "invalid"}).status_code == 400


def test_oversized_websocket_frame_is_closed(tmp_path, monkeypatch):
    app = make_app(tmp_path, monkeypatch)
    with TestClient(app, base_url="http://127.0.0.1") as client:
        with client.websocket_connect("ws://127.0.0.1/test-websocket") as ws:
            assert ws.receive_text() == "ready"
            ws.send_bytes(b"x" * (1024 * 1024 + 1))
            with pytest.raises(WebSocketDisconnect) as closed:
                ws.receive_text()
            assert closed.value.code == 1009


def test_non_loopback_launch_requires_authentication(tmp_path, monkeypatch):
    make_app(tmp_path, monkeypatch)
    from voiceservice.__main__ import main

    monkeypatch.setattr("sys.argv", ["voiceservice", "--config", str(tmp_path / "config.yaml"), "--host", "0.0.0.0"])
    called = []
    monkeypatch.setattr("uvicorn.run", lambda *args, **kwargs: called.append(kwargs))
    with pytest.raises(SystemExit):
        main()
    assert called == []
