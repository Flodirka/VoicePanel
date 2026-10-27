"""FastAPI application factory."""
from __future__ import annotations

from contextlib import asynccontextmanager
from pathlib import Path
from fastapi import FastAPI

from . import __version__, compute
from .api import admin, openai, sessions, text, voices
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
    app.include_router(admin.router)
    return app
