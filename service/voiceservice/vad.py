"""Silero VAD (ONNX) and Pithagoras-style speculative live transcription.

The VAD consumes 512-sample (32 ms) frames of 16 kHz mono float32 audio and
returns speech probabilities. `LiveTranscription` mirrors the technique used
by Pithagoras: pre-roll buffering, revision counting, speculative whole-segment
requests whose result is reused when the final revision matches.
"""
from __future__ import annotations

import asyncio
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

FRAME_SAMPLES = 512  # Silero v5 frame size at 16 kHz (32 ms)


class SileroVAD:
    """Stateful Silero VAD v5 session for one audio stream.

    Follows the official OnnxWrapper contract: each call feeds the model
    ``context(64) + frame(512)`` samples at 16 kHz and keeps the LSTM state.
    """

    def __init__(self, model_path: str) -> None:
        import onnxruntime as ort

        opts = ort.SessionOptions()
        opts.inter_op_num_threads = 1
        opts.intra_op_num_threads = 1
        self.session = ort.InferenceSession(
            str(model_path), sess_options=opts, providers=["CPUExecutionProvider"]
        )
        self.reset()

    def reset(self) -> None:
        self._state = np.zeros((2, 1, 128), dtype=np.float32)
        self._context = np.zeros(64, dtype=np.float32)

    def probability(self, frame: np.ndarray) -> float:
        """frame: 512 float32 samples at 16 kHz."""
        if len(frame) < FRAME_SAMPLES:
            frame = np.pad(frame, (0, FRAME_SAMPLES - len(frame)))
        x = np.concatenate([self._context, frame]).reshape(1, -1).astype(np.float32)
        ort_inputs = {
            "input": x,
            "state": self._state,
            "sr": np.array(16000, dtype=np.int64),
        }
        out, self._state = self.session.run(None, ort_inputs)
        self._context = x[0, -64:].copy()
        return float(out[0][0])


@dataclass(eq=False)
class Segment:
    frames: list[np.ndarray]
    revision: int = 1
    quiet: int = 0
    confirmed: bool = False
    requested: int = -1
    last_request: int = 0
    result_text: str | None = None
    result_revision: int = -1
    task: asyncio.Task | None = field(default=None, repr=False)

    def samples(self) -> np.ndarray:
        return np.concatenate(self.frames) if self.frames else np.zeros(0, dtype=np.float32)


class LiveTranscription:
    """Speculative transcription over VAD-gated segments.

    Callbacks:
      * request(samples, cancellation) -> str  : synchronous ASR of the segment so far
      * preview(text)                          : partial transcript display
    """

    def __init__(
        self,
        request,
        preview,
        *,
        speech_threshold: float = 0.5,
        preroll_frames: int = 11,
        min_speech_frames: int = 6,
        speculate_every_frames: int = 63,
        speculative: bool = True,
    ) -> None:
        self.request = request
        self.preview = preview
        self.speech_threshold = speech_threshold
        self.preroll_frames = preroll_frames
        self.min_speech_frames = min_speech_frames
        self.speculate_every_frames = speculate_every_frames
        self.speculative = speculative

        self._preroll: deque[np.ndarray] = deque(maxlen=preroll_frames)
        self.current: Segment | None = None
        self._segments: set[Segment] = set()
        self._finalized: list[tuple[np.ndarray, Segment]] = []

    # -- VAD events ------------------------------------------------------
    def frame(self, probability: float, samples: np.ndarray) -> None:
        frame = samples.copy()
        segment = self.current
        if segment is None:
            self._preroll.append(frame)
            return
        segment.frames.append(frame)
        if probability >= self.speech_threshold:
            segment.revision += 1
            segment.quiet = 0
        else:
            segment.quiet += 1

    def speech_started(self) -> Segment | None:
        if self.current is not None:
            return None
        segment = Segment(frames=list(self._preroll))
        self._preroll.clear()
        self.current = segment
        self._segments.add(segment)
        return segment

    def speech_ended(self) -> Segment | None:
        segment = self.current
        self.current = None
        self._preroll.clear()
        return segment

    def speculative_due(self, segment: Segment) -> bool:
        if not self.speculative or not segment.confirmed:
            return False
        if segment.task is not None and not segment.task.done():
            return False
        return segment.quiet >= 6 or (len(segment.frames) - segment.last_request) >= self.speculate_every_frames

    def launch_speculative(self, segment: Segment) -> None:
        revision = segment.revision
        segment.requested = revision
        segment.last_request = len(segment.frames)

        async def run() -> None:
            try:
                text = await self.request(segment.samples())
            except asyncio.CancelledError:
                raise
            except Exception:
                return  # a speculative failure is retried by the final request
            segment.result_text = text
            segment.result_revision = revision
            if self.current is segment:
                self.preview(text or "")

        segment.task = asyncio.create_task(run())

    async def finish(self, segment: Segment) -> str:
        """Authoritative transcript for an ended segment; reuses a matching speculation."""
        try:
            if segment.task is not None:
                try:
                    await segment.task
                except asyncio.CancelledError:
                    pass
            if segment.result_revision == segment.revision and (segment.result_text or "").strip():
                return segment.result_text or ""
            return await self.request(segment.samples())
        finally:
            self._segments.discard(segment)
            if segment.task is not None and not segment.task.done():
                segment.task.cancel()

    def reset(self) -> None:
        for segment in list(self._segments):
            if segment.task is not None and not segment.task.done():
                segment.task.cancel()
        self._segments.clear()
        self.current = None
        self._preroll.clear()
