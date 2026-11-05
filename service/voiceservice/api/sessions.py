"""Continuous dictation over WebSocket with server-side VAD.

Protocol (client -> server):
  * binary frames: PCM16LE mono 16 kHz audio chunks (any size; re-chunked to 512)
  * text frames (JSON):
      {"type":"config", "model": "nemotron", "language": "auto", ...}   optional
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


@router.websocket("/v1/sessions/stream")
async def stream_session(ws: WebSocket) -> None:
    """Relay live PCM to the engine and push transcript events back.

    The panel sends 16 kHz mono PCM16 frames as binary messages and reads JSON
    events. The engine keeps the encoder work it has already done, so the final
    arrives shortly after the last frame instead of after a full batch run
    (measured 465-523 ms against 2.6-3.0 s for a 6.4 s utterance on CPU).

    Client -> server:
      * binary frames: PCM16LE mono at the declared sample rate
      * {"type":"finish"} closes the stream and waits for the final event
      * {"type":"cancel"} drops everything
    Server -> client:
      * {"type":"ready","model":...,"sample_rate":...}
      * the engine's own events (transcript.text.delta / .done) verbatim
      * {"type":"done"} after the final, {"type":"error","message":...} on failure
    """
    await ws.accept()
    registry = ws.app.state.registry
    params = ws.query_params
    language = params.get("language") or None
    try:
        sample_rate = int(params.get("sample_rate", 16000))
    except ValueError:
        sample_rate = 16000
    try:
        backend, model_id = registry.resolve_asr(params.get("model"))
    except KeyError as exc:
        await ws.send_json({"type": "error", "message": str(exc)})
        await ws.close()
        return
    if backend != "audiocpp":
        await ws.send_json({"type": "error", "message": "streaming needs an audio.cpp model"})
        await ws.close()
        return
    entry = registry.audiocpp.models.get(model_id, {})
    if entry.get("mode") != "streaming":
        await ws.send_json({
            "type": "error",
            "message": (
                f"model '{model_id}' is configured with mode: "
                f"{entry.get('mode', 'offline')}; set mode: streaming to use this route"
            ),
        })
        await ws.close()
        return

    queue: asyncio.Queue[bytes | None] = asyncio.Queue()
    finished = asyncio.Event()

    async def pcm_source():
        while True:
            item = await queue.get()
            if item is None:
                return
            yield item

    async def pump() -> None:
        try:
            async for line in registry.audiocpp.transcribe_live(
                pcm_source(),
                model=model_id,
                language=language,
                sample_rate=sample_rate,
                channels=1,
            ):
                if not line.startswith("data:"):
                    continue
                payload = line[5:].strip()
                if payload == "[DONE]":
                    break
                try:
                    event = json.loads(payload)
                except ValueError:
                    continue
                await ws.send_json(event)
            await ws.send_json({"type": "done"})
        except asyncio.CancelledError:
            raise
        except Exception as exc:  # noqa: BLE001 - reported to the client, session survives
            try:
                await ws.send_json({"type": "error", "message": str(exc)})
            except Exception:
                pass
        finally:
            finished.set()

    await ws.send_json({"type": "ready", "model": model_id, "sample_rate": sample_rate})
    async with registry._asr_lock:      # one live stream at a time, as for batch requests
        task = asyncio.create_task(pump())
        try:
            while True:
                message = await ws.receive()
                if message.get("type") == "websocket.disconnect":
                    break
                if (data := message.get("bytes")) is not None:
                    if data:
                        queue.put_nowait(data)
                    continue
                text = message.get("text") or ""
                if not text:
                    continue
                try:
                    control = json.loads(text)
                except ValueError:
                    await ws.send_json({"type": "error", "message": "invalid JSON control frame"})
                    continue
                kind = control.get("type")
                if kind in ("finish", "close", "eos"):
                    queue.put_nowait(None)
                    try:
                        await asyncio.wait_for(finished.wait(), timeout=120)
                    except asyncio.TimeoutError:
                        await ws.send_json({"type": "error", "message": "no final from the engine"})
                    break
                if kind == "cancel":
                    queue.put_nowait(None)
                    break
                if kind == "config" and control.get("language"):
                    language = str(control["language"])
        except WebSocketDisconnect:
            pass
        finally:
            queue.put_nowait(None)
            if not task.done():
                try:
                    await asyncio.wait_for(task, timeout=10)
                except (asyncio.TimeoutError, asyncio.CancelledError):
                    task.cancel()


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
        asr_model = (cfg.get("asr_model") or (model_cfg or {}).get("id") or registry.cfg["asr"]["default"])
    except Exception:
        asr_model = "nemotron"
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

    min_speech_frames = int(cfg.get("min_speech_frames", 6))
    live = LiveTranscription(
        asr_request,
        preview,
        speech_threshold=float(cfg.get("speech_threshold", 0.5)),
        preroll_frames=int(cfg.get("preroll_frames", 11)),
        min_speech_frames=min_speech_frames,
        speculate_every_frames=int(cfg.get("speculate_every_frames", 63)),
        speculative=bool(cfg.get("speculative", True)),
    )

    speech_start_s: float | None = None
    buffer = np.zeros(0, dtype=np.float32)
    max_utterance_s = float(cfg.get("max_utterance_s", 30.0))
    silence_frames_needed = int(cfg.get("silence_frames", 24))

    # Finals are recognised in the background so audio intake and VAD keep
    # running while an utterance is transcribed. A chain of sender tasks keeps
    # the final events in utterance order.
    last_sender: asyncio.Task | None = None
    pending: set[asyncio.Task] = set()

    def _track(task: asyncio.Task) -> asyncio.Task:
        pending.add(task)
        task.add_done_callback(pending.discard)
        return task

    async def finalize(segment) -> None:
        nonlocal speech_start_s, last_sender
        if segment is None:
            return
        speech_start_s = None
        recognition = _track(asyncio.create_task(live.finish(segment)))
        previous = last_sender

        async def send_in_order() -> None:
            if previous is not None:
                await previous
            try:
                text = await recognition
            except asyncio.CancelledError:
                raise
            except Exception as exc:  # noqa: BLE001 - reported, the session continues
                try:
                    await ws.send_json({"type": "error", "message": f"transcription failed: {exc}"})
                except Exception:
                    pass
                return
            try:
                await ws.send_json({"type": "final", "text": text, "t_offset_s": t_offset()})
            except Exception:
                pass

        last_sender = _track(asyncio.create_task(send_in_order()))

    async def drain_finals() -> None:
        if last_sender is not None:
            await last_sender

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
                    await drain_finals()
                    await ws.send_json({"type": "done"})
                    break
                elif mtype == "cancel":
                    for task in list(pending):
                        task.cancel()
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
        for task in list(pending):
            task.cancel()
        live.reset()
