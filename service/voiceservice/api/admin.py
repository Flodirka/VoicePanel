"""Health, capability discovery, and administration endpoints."""
from __future__ import annotations

import asyncio
import os
import time
from typing import Any

import psutil
from fastapi import APIRouter, Depends, HTTPException, Request
from fastapi.responses import JSONResponse
from pydantic import BaseModel, Field

from .. import __version__, compute
from ..backends import ComputeBusy, Registry
from .deps import get_registry

router = APIRouter()
_started_at = time.monotonic()


def _gpu_stats() -> dict[str, Any] | None:
    try:
        import subprocess
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,memory.total,memory.used,utilization.gpu",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=4,
        )
        if out.returncode != 0 or not out.stdout.strip():
            return None
        name, total, used, util = [p.strip() for p in out.stdout.splitlines()[0].split(",")]
        return {"name": name, "total_mib": int(total), "used_mib": int(used), "utilization_pct": int(util)}
    except Exception:
        return None


@router.get("/health")
async def health(registry: Registry = Depends(get_registry)) -> dict[str, Any]:
    """Liveness of the gateway itself (cheap; no backend checks)."""
    return {
        "status": "ok",
        "version": __version__,
        "uptime_s": round(time.monotonic() - _started_at, 1),
    }


@router.get("/v1/capabilities")
async def capabilities(registry: Registry = Depends(get_registry)) -> dict[str, Any]:
    """Readiness of each inference capability and its backends."""
    audiocpp_models = await registry.audiocpp.loaded_models()
    live = {m["id"]: m for m in audiocpp_models}
    configured = registry.cfg["backends"]["audiocpp"].get("models", [])
    asr_entries = [
        {
            "id": m["id"],
            "engine": "audiocpp",
            "loaded": bool(live.get(m["id"], {}).get("loaded")),
            "task": "asr",
            "model": os.path.basename(m["path"]),
        }
        for m in configured
        if m.get("task") == "asr"
    ]
    if registry.whisper is not None:
        asr_entries.append({
            "id": "whisper",
            "engine": "whisper.cpp",
            "loaded": registry.whisper.proc.running,
            "task": "asr",
            "model": os.path.basename(registry.whisper.cfg["model_path"]),
        })
    tts_entries = [
        {
            "id": m["id"],
            "engine": "audiocpp",
            "loaded": bool(live.get(m["id"], {}).get("loaded")),
            "task": "tts",
            "model": os.path.basename(m["path"]),
        }
        for m in configured
        if m.get("task") == "tts"
    ]
    if registry.audiocpp_clone is not None:
        clone_live = {m["id"]: m for m in await registry.audiocpp_clone.loaded_models()}
        tts_entries += [
            {
                "id": m["id"],
                "engine": "audiocpp-clone",
                "loaded": bool(clone_live.get(m["id"], {}).get("loaded")),
                "task": "tts",
                "model": os.path.basename(m["path"]),
                "on_demand": True,
            }
            for m in registry.audiocpp_clone.cfg.get("models", [])
            if m.get("task") == "tts"
        ]
    llm_entry = None
    if registry.llm is not None:
        llm_entry = {
            "id": registry.llm.model_alias,
            "engine": "llama.cpp",
            "loaded": registry.llama.proc.running,
            "task": "llm",
            "model": os.path.basename(registry.llm.cfg["model_path"]),
        }
    return {
        "version": __version__,
        "asr": {"default": registry.cfg["asr"]["default"], "models": asr_entries},
        "tts": {"default": registry.cfg["tts"].get("default", "auto"), "models": tts_entries},
        "llm": llm_entry,
        "compute": _compute_profiles(registry),
        "processes": await registry.status(),
        "host": {"ram_total_mib": int(psutil.virtual_memory().total / 1048576),
                 "ram_used_mib": int(psutil.virtual_memory().used / 1048576)},
        "gpu": _gpu_stats(),
    }


def _compute_profiles(registry: Registry) -> dict[str, Any]:
    """Which process runs on which device, so clients can show it without reading config."""
    def describe(section: dict[str, Any] | None) -> dict[str, Any] | None:
        if not section:
            return None
        return {
            "backend": section.get("backend", "cpu"),
            "binary": os.path.basename(str(section.get("binary", ""))),
            "idle_stop_s": section.get("idle_stop_s"),
        }

    bcfg = registry.cfg.get("backends", {})
    llama = bcfg.get("llama", {})
    gpu_layers = int(llama.get("gpu_layers", 0) or 0)
    return {
        "main": describe(bcfg.get("audiocpp")),          # ASR + built-in voices
        "clone": describe(bcfg.get("audiocpp_clone")) if registry.audiocpp_clone else None,
        "llm": {"backend": "gpu" if gpu_layers > 0 else "cpu", "gpu_layers": gpu_layers},
        "whisper": {"backend": "cpu", "binary": os.path.basename(str((bcfg.get("whisper") or {}).get("binary", "")))} if registry.whisper else None,
    }


@router.post("/admin/backends/{name}/load")
async def load_backend(name: str, registry: Registry = Depends(get_registry)):
    proc = _backend_proc(registry, name)
    await proc.start()
    return {"status": "started", **proc.status()}


@router.post("/admin/backends/{name}/unload")
async def unload_backend(name: str, registry: Registry = Depends(get_registry)):
    proc = _backend_proc(registry, name)
    await proc.stop()
    return {"status": "stopped", **proc.status()}


@router.post("/admin/backends/{name}/restart")
async def restart_backend(name: str, registry: Registry = Depends(get_registry)):
    proc = _backend_proc(registry, name)
    await proc.stop()
    await proc.start()
    return {"status": "restarted", **proc.status()}


@router.get("/admin/status")
async def admin_status(registry: Registry = Depends(get_registry)) -> dict[str, Any]:
    return {"service": await health(registry), "backends": await registry.status(), "gpu": _gpu_stats()}


class ComputeRequest(BaseModel):
    devices: dict[str, str] = Field(min_length=1)


@router.get("/admin/compute")
async def get_compute(registry: Registry = Depends(get_registry)) -> dict[str, Any]:
    """Device choices for the three inference processes, plus their state."""
    return _compute_state(registry)


@router.post("/admin/compute")
async def set_compute(req: ComputeRequest, registry: Registry = Depends(get_registry)):
    """Persist new device choices and apply them to the idle processes.

    A process that is serving a request is never stopped underneath it: the
    call fails with 409 and nothing is changed or saved.
    """
    try:
        await registry.apply_compute(req.devices)
    except ComputeBusy as exc:
        raise HTTPException(409, f"service is busy: {exc}")
    except ValueError as exc:
        raise HTTPException(400, str(exc))
    return _compute_state(registry)


def _compute_state(registry: Registry) -> dict[str, Any]:
    current = compute.current_devices(registry.cfg)
    available = compute.available_devices(registry.cfg)
    groups = []
    for group, meta in compute.GROUPS.items():
        proc = registry.backend_process(group)
        status = proc.status() if proc is not None else None
        groups.append({
            "id": group,
            "label": meta["label"],
            "process": meta["process"],
            "device": current[group],
            "available": available[group],
            "running": bool(status and status["running"]),
            "busy": bool(status and status["leases"] > 0),
            "adopted": bool(status and status["adopted"]),
        })
    return {
        "groups": groups,
        "storage": str(compute.state_path(registry.cfg["_root"])),
    }


@router.post("/admin/shutdown")
async def shutdown(request: Request, registry: Registry = Depends(get_registry)):
    """Graceful shutdown: stop owned inference processes, then exit."""

    async def _stop_and_exit() -> None:
        await asyncio.sleep(0.2)
        await registry.stop_all()
        os._exit(0)

    asyncio.create_task(_stop_and_exit())
    return {"status": "shutting_down"}


def _backend_proc(registry: Registry, name: str):
    mapping = {
        "audiocpp": registry.audiocpp.proc,
        "audiocpp_clone": registry.audiocpp_clone.proc if registry.audiocpp_clone else None,
        "clone": registry.audiocpp_clone.proc if registry.audiocpp_clone else None,
        "whisper": registry.whisper.proc if registry.whisper else None,
        "llama": registry.llama.proc if registry.llm else None,
    }
    proc = mapping.get(name)
    if proc is None:
        raise HTTPException(404, f"unknown backend '{name}'; use one of {', '.join(k for k, v in mapping.items() if v)}")
    return proc
