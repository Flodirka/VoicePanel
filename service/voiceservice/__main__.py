"""Entry point: python -m voiceservice [--config path] [--host H] [--port P]"""
from __future__ import annotations

import argparse
import os

import uvicorn


def main() -> None:
    parser = argparse.ArgumentParser(description="Local Voice AI Service")
    parser.add_argument("--config", default=None, help="path to config.yaml")
    parser.add_argument("--host", default=None, help="override bind host (default 127.0.0.1)")
    parser.add_argument("--port", type=int, default=None, help="override bind port (default 8080)")
    args = parser.parse_args()

    # Locate config relative to this file when not given.
    config_path = args.config
    if config_path is None:
        for candidate in (
            os.path.join(os.getcwd(), "config.yaml"),
            os.path.join(os.path.dirname(__file__), "..", "..", "config.yaml"),
        ):
            if os.path.isfile(candidate):
                config_path = os.path.abspath(candidate)
                break

    from .app import create_app
    from .config import load_config
    from .security import service_token

    cfg = load_config(config_path)
    host = args.host or cfg["service"]["host"]
    port = args.port or int(cfg["service"]["port"])
    if host not in ("127.0.0.1", "localhost", "::1"):
        if not service_token(cfg):
            parser.error("non-loopback binding requires an authentication token")
        if host in ("0.0.0.0", "::"):
            parser.error("bind a specific interface address instead of a wildcard")
    app = create_app(config_path)
    app.state.cfg["service"]["host"] = host
    uvicorn.run(app, host=host, port=port, log_level=cfg["logging"].get("level", "info"), ws_max_size=1024 * 1024)


if __name__ == "__main__":
    main()
