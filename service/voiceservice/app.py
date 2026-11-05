"""FastAPI application factory."""
from __future__ import annotations

import logging
from contextlib import asynccontextmanager
from pathlib import Path
from fastapi import FastAPI

from . import __version__, compute
from .api import admin, openai, sessions, text, voices, warmup
from .backends import Registry
from .config import load_config
from .voices import VoiceStore
from .security import LocalSecurityMiddleware


def create_app(config_path: str | Path | None = None) -> FastAPI:
    cfg = load_config(config_path)
    compute.apply_overrides(cfg)   # saved CPU/GPU choices win over config.yaml
    root = Path(cfg["_root"])
    state_dir = root / "runtime" / "state"
    log_dir = root / "runtime" / "logs"

    @asynccontextmanager
    async def lifespan(app: FastAPI):
        # After uvicorn has configured logging, so the line is actually printed.
        _log_effective_profile(cfg)
        app.state.registry = Registry(cfg, state_dir, log_dir)
        app.state.voices = VoiceStore(root / "runtime" / "voice-library")
        yield
        await app.state.registry.stop_all()

    app = FastAPI(
        title="Voice AI Service",
        version=__version__,
        lifespan=lifespan,
        docs_url=None,   # local-only service; keep the surface minimal
        redoc_url=None,
        openapi_url=None,
    )
    app.state.cfg = cfg

    app.add_middleware(LocalSecurityMiddleware, cfg=cfg)
    app.include_router(openai.router)
    app.include_router(text.router)
    app.include_router(voices.router)
    app.include_router(sessions.router)
    app.include_router(warmup.router)
    app.include_router(admin.router)
    return app


def _log_effective_profile(cfg: dict) -> None:
    """Say which build each engine group will actually run.

    config.yaml and runtime/state/compute.json can disagree (the saved choice
    wins), so the profile that will really be used is worth printing once at
    startup instead of being rediscovered by reading the source.
    """
    # uvicorn already configures handlers and the level; a bare package logger
    # would swallow the line.
    log = logging.getLogger("uvicorn.error")
    devices = compute.current_devices(cfg)
    backends = cfg.get("backends", {})
    detail = []
    for group, device in devices.items():
        if group == "llm":
            binary = (backends.get("llama") or {}).get("binary", "")
            extra = f"gpu_layers={(backends.get('llama') or {}).get('gpu_layers')}"
        else:
            key = {"clone": "audiocpp_clone", "diar": "audiocpp_diar"}.get(group, "audiocpp")
            binary = (backends.get(key) or {}).get("binary", "")
            extra = str((backends.get(key) or {}).get("backend", ""))
        detail.append(f"{group}={device} ({Path(str(binary)).parent.name}/{extra})")
    log.info("effective compute profile: %s", ", ".join(detail))
    if any(value == "gpu" for value in devices.values()):
        log.info(
            "a GPU group is selected; if another application holds the card, "
            "expect slower inference than the CPU profile"
        )
