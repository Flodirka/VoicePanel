"""Shared HTTP/WebSocket boundary for the local service."""
from __future__ import annotations

import os
from secrets import compare_digest
from urllib.parse import urlsplit

from starlette.datastructures import Headers
from starlette.exceptions import HTTPException
from starlette.responses import JSONResponse
from starlette.websockets import WebSocketDisconnect


def service_token(cfg: dict) -> str:
    return str(cfg["service"].get("auth_token") or os.environ.get("VOICE_SERVICE_AUTH_TOKEN", "")).strip()


class LocalSecurityMiddleware:
    def __init__(self, app, cfg: dict) -> None:
        self.app = app
        self.token = service_token(cfg).encode("utf-8")
        self.origins = set(cfg["service"].get("allow_origins") or [])
        self.hosts = {"127.0.0.1", "localhost", "::1"}
        configured = str(cfg["service"].get("host", "127.0.0.1")).lower()
        if configured not in {"0.0.0.0", "::"}:
            self.hosts.add(configured)
        self.body_limit = max(1, int(cfg["service"].get("max_upload_mb", 200))) * 1024 * 1024
        self.frame_limit = min(self.body_limit, 1024 * 1024)

    async def __call__(self, scope, receive, send) -> None:
        kind = scope["type"]
        if kind not in {"http", "websocket"}:
            await self.app(scope, receive, send)
            return
        headers = Headers(scope=scope)
        try:
            host = urlsplit("//" + headers.get("host", "")).hostname
        except ValueError:
            host = None
        status, code = 0, ""
        if host not in self.hosts:
            status, code = 400, "host_forbidden"
        elif headers.get("origin") is not None and headers["origin"] not in self.origins:
            status, code = 403, "origin_forbidden"
        elif self.token and not (
            compare_digest(headers.get("authorization", "").encode("utf-8"), b"Bearer " + self.token)
            or compare_digest(headers.get("x-api-key", "").encode("utf-8"), self.token)
        ):
            status, code = 401, "unauthorized"
        if status:
            if kind == "websocket":
                await send({"type": "websocket.close", "code": 1008})
            else:
                await JSONResponse({"error": {"code": code}}, status_code=status)(scope, receive, send)
            return
        if kind == "http" and "content-length" in headers:
            try:
                length = int(headers["content-length"])
                if length < 0:
                    raise ValueError
            except ValueError:
                await JSONResponse({"error": {"code": "invalid_content_length"}}, status_code=400)(scope, receive, send)
                return
            if length > self.body_limit:
                await JSONResponse({"error": {"code": "body_too_large"}}, status_code=413)(scope, receive, send)
                return

        consumed = 0

        async def limited_receive():
            nonlocal consumed
            message = await receive()
            if message["type"] == "http.request":
                consumed += len(message.get("body", b""))
                if consumed > self.body_limit:
                    raise HTTPException(413, "Request body too large")
            elif message["type"] == "websocket.receive":
                size = len(message.get("bytes") or (message.get("text") or "").encode("utf-8"))
                if size > self.frame_limit:
                    await send({"type": "websocket.close", "code": 1009})
                    raise WebSocketDisconnect(1009)
            return message

        try:
            await self.app(scope, limited_receive, send)
        except WebSocketDisconnect:
            if kind != "websocket":
                raise
