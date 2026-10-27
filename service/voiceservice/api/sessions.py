"""Continuous dictation over WebSocket with server-side VAD.

Protocol (client -> server):
  * binary frames: PCM16LE mono 16 kHz audio chunks (any size; re-chunked to 512)
  * text frames (JSON):
      {"type":"config", "model": "qwen3-asr", "language": "auto", ...}   optional
      {"type":"pause"} / {"type":"resume"}
      {"type":"finish"}   end the session; pending audio is finalized
      {"type":"cancel"}   drop everything and close

Server -> client (JSON):
  * {"type":"speech_started"}
  * {"type":"partial","text":"...","t_offset_s":...}
  * {"type":"final","text":"...","start_s":...,"end_s":...}
  * {"type":"error","message":"..."}
"""
from __future__ import annotations

import asyncio
import json
import time
from pathlib import Path
from typing import Any

import numpy as np
from fastapi import APIRouter, WebSocket, WebSocketDisconnect

from ..audio import pcm16_to_wav, samples_to_wav
from ..processing import clean_transcript
from ..vad import FRAME_SAMPLES, LiveTranscription, SileroVAD

router = APIRouter()


@router.websocket("/v1/sessions/dictation")
async def dictation_session(ws: WebSocket) -> None:
    await ws.accept()
    app = ws.app
    registry = app.state.registry
    cfg: dict[str, Any] = dict(app.state.cfg.get("sessions", {}))
    vad_model = cfg.get("vad_model")
    if not vad_model or not Path(vad_model).is_file():
        await ws.send_json({"type": "error", "message": f"VAD model missing: {vad_model}"})
        await ws.close()
        return

    try:
        model_cfg = None
        for m in registry.cfg["backends"]["audiocpp"].get("models", []):
            if m.get("task") == "asr":
                model_cfg = m
                if m["id"] == cfg.get("asr_model"):
                    break
        asr_model = (cfg.get("asr_model") or (model_cfg or {}).get("id") or "qwen3-asr")
    except Exception:
        asr_model = "qwen3-asr"
    language: str | None = None
    paused = False
    session_started = time.monotonic()

    vad = SileroVAD(vad_model)
    loop = asyncio.get_running_loop()

    def t_offset() -> float:
        return round(time.monotonic() - session_started, 2)

    async def asr_request(samples: np.ndarray) -> str:
        if len(samples) < 1600:  # <100 ms: nothing to recognise
            return ""
        wav = samples_to_wav(samples)
        async with registry._asr_lock:
            data = await registry.audiocpp.transcribe(wav, model=asr_model, language=language)
        return clean_transcript(data.get("text", ""))

    async def preview(text: str) -> None:
        try:
            await ws.send_json({"type": "partial", "text": text, "t_offset_s": t_offset()})
        except Exception:
            pass

    live = LiveTranscription(
        asr_request,
        preview,
        speech_threshold=float(cfg.get("speech_threshold", 0.5)),
        preroll_frames=int(cfg.get("preroll_frames", 11)),
        speculate_every_frames=int(cfg.get("speculate_every_frames", 63)),
        speculative=bool(cfg.get("speculative", True)),
    )

    speech_start_s: float | None = None
    buffer = np.zeros(0, dtype=np.float32)
    max_utterance_s = float(cfg.get("max_utterance_s", 30.0))
    min_speech_frames = int(cfg.get("min_speech_frames", 6))
    silence_frames_needed = int(cfg.get("silence_frames", 24))

    async def finalize(segment) -> None:
        nonlocal speech_start_s
        if segment is None:
            return
        text = await live.finish(segment)
        speech_start_s = None
        try:
            await ws.send_json({"type": "final", "text": text, "t_offset_s": t_offset()})
        except Exception:
            pass

    async def process_frame(frame: np.ndarray) -> None:
        nonlocal speech_start_s, buffer
        prob = await loop.run_in_executor(None, vad.probability, frame)
        if live.current is None:
            if prob >= live.speech_threshold:
                segment = live.speech_started()
                speech_start_s = t_offset()
                if segment is not None:
                    try:
                        await ws.send_json({"type": "speech_started", "t_offset_s": speech_start_s})
                    except Exception:
                        pass
                live.frame(prob, frame)
            else:
                # keep Silero state warm and drop the audio
                return
        else:
            live.frame(prob, frame)
            segment = live.current
            if segment is not None and segment.quiet == 0 and speech_start_s is None:
                speech_start_s = t_offset()
            # finalise after enough silence
            if segment is not None and segment.quiet >= silence_frames_needed and len(segment.frames) - segment.quiet >= min_speech_frames:
                ended = live.speech_ended()
                await finalize(ended)
            elif segment is not None and len(segment.frames) * (FRAME_SAMPLES / 16000.0) >= max_utterance_s:
                ended = live.speech_ended()
                await finalize(ended)
            elif segment is not None and live.speculative_due(segment):
                live.launch_speculative(segment)

    try:
        await ws.send_json({"type": "ready", "asr_model": asr_model, "frame_samples": FRAME_SAMPLES})
        while True:
            message = await ws.receive()
            if message.get("type") == "websocket.disconnect":
                break
            if (data := message.get("bytes")) is not None:
                if paused:
                    continue
                pcm = np.frombuffer(data, dtype="<i2").astype(np.float32) / 32768.0
                buffer = np.concatenate([buffer, pcm])
                while len(buffer) >= FRAME_SAMPLES:
                    frame, buffer = buffer[:FRAME_SAMPLES], buffer[FRAME_SAMPLES:]
                    await process_frame(frame)
            elif (text := message.get("text")) is not None:
                try:
                    msg = json.loads(text)
                except json.JSONDecodeError:
                    await ws.send_json({"type": "error", "message": "invalid JSON control frame"})
                    continue
                mtype = msg.get("type")
                if mtype == "config":
                    language = msg.get("language") or language
                    if msg.get("model"):
                        try:
                            _, asr_model = registry.resolve_asr(str(msg["model"]))
                        except KeyError as exc:
                            await ws.send_json({"type": "error", "message": str(exc)})
                    if "speech_threshold" in msg:
                        live.speech_threshold = float(msg["speech_threshold"])
                    if "silence_frames" in msg:
                        silence_frames_needed = max(1, int(msg["silence_frames"]))
                    await ws.send_json({"type": "config_applied"})
                elif mtype == "pause":
                    paused = True
                elif mtype == "resume":
                    paused = False
                    vad.reset()
                elif mtype == "finish":
                    if live.current is not None:
                        ended = live.speech_ended()
                        await finalize(ended)
                    await ws.send_json({"type": "done"})
                    break
                elif mtype == "cancel":
                    live.reset()
                    vad.reset()
                    await ws.send_json({"type": "done"})
                    break
                else:
                    await ws.send_json({"type": "error", "message": f"unknown control type '{mtype}'"})
    except WebSocketDisconnect:
        pass
    except Exception as exc:  # noqa: BLE001
        try:
            await ws.send_json({"type": "error", "message": str(exc)})
        except Exception:
            pass
    finally:
        live.reset()
