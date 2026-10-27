"""An external language model: adopt a running server, never start or stop it."""
from __future__ import annotations

import asyncio
import threading
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

import pytest

from voiceservice.backends.process import ManagedProcess, ProcessError


class _Health(BaseHTTPRequestHandler):
    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler API
        self.send_response(200)
        self.send_header("Content-Length", "15")
        self.end_headers()
        self.wfile.write(b'{"status":"ok"}')

    def log_message(self, *args) -> None:  # keep the test output clean
        pass


@pytest.fixture()
def health_server():
    server = ThreadingHTTPServer(("127.0.0.1", 0), _Health)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    yield server
    server.shutdown()
    server.server_close()


def test_external_server_is_adopted_and_left_running(health_server) -> None:
    port = health_server.server_address[1]
    proc = ManagedProcess(name="llama", command=["not-a-real-binary"], host="127.0.0.1",
                          port=port, spawn=False)
    asyncio.run(proc.start())
    status = proc.status()
    assert status["running"] and status["adopted"]
    assert status["pid"] is None, "an adopted server is not ours to manage"

    asyncio.run(proc.stop())
    # Someone else's process must survive our shutdown.
    with urllib.request.urlopen(f"http://127.0.0.1:{port}/health", timeout=5) as response:
        assert response.status == 200


def test_external_server_that_is_down_fails_loudly() -> None:
    proc = ManagedProcess(name="llama", command=["not-a-real-binary"], host="127.0.0.1",
                          port=65533, spawn=False, start_timeout_s=1)
    with pytest.raises(ProcessError, match="no server answering"):
        asyncio.run(proc.start())
