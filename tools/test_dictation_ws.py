"""Functional test of /v1/sessions/dictation: streams a WAV as PCM chunks and
prints the events (speech_started / partial / final)."""
import asyncio
import json
import sys
import wave

import websockets

WS_URL = "ws://127.0.0.1:8080/v1/sessions/dictation"


async def main(wav_path: str) -> int:
    with wave.open(wav_path, "rb") as w:
        assert w.getframerate() == 16000, f"need 16 kHz wav, got {w.getframerate()}"
        pcm = w.readframes(w.getnframes())
    finals: list[str] = []
    async with websockets.connect(WS_URL, max_size=2**22) as ws:
        print("ready:", await ws.recv())
        chunk = 16000 * 2 // 10  # 100 ms of PCM16 mono
        send_iter = iter(range(0, len(pcm), chunk))

        async def feed() -> None:
            for i in send_iter:
                await ws.send(pcm[i : i + chunk])
                await asyncio.sleep(0.02)   # ~5x faster than realtime
            # trailing silence so the VAD finalises the utterance
            await ws.send(b"\x00" * (16000 * 2 * 2))
            await ws.send(json.dumps({"type": "finish"}))

        feeder = asyncio.create_task(feed())
        try:
            while True:
                msg = await asyncio.wait_for(ws.recv(), timeout=90)
                data = json.loads(msg)
                print("EVT:", json.dumps(data, ensure_ascii=False)[:200], flush=True)
                if data.get("type") == "final":
                    finals.append(data.get("text", ""))
                if data.get("type") == "done":
                    break
        except (asyncio.TimeoutError, websockets.ConnectionClosed):
            pass
        finally:
            feeder.cancel()
    print("finals:", finals)
    return 0 if finals else 1


if __name__ == "__main__":
    raise SystemExit(asyncio.run(main(sys.argv[1] if len(sys.argv) > 1 else "smoke-ru.wav")))
