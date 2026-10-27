"""Shared FastAPI dependencies."""
from __future__ import annotations

from fastapi import Request

from ..backends import Registry
from ..voices import VoiceStore


def get_registry(request: Request) -> Registry:
    return request.app.state.registry


def get_voices(request: Request) -> VoiceStore:
    return request.app.state.voices
