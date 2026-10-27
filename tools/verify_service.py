"""End-to-end verification for a running Voice AI Service.

Checks ASR, built-in TTS (Kokoro/Piper), zero-shot cloned-voice synthesis in
Russian and English, and reports latency plus GPU/RAM from /admin/status.

Usage:
    python tools/verify_service.py [--base http://127.0.0.1:8080]
                                   [--voice example-voice]
                                   [--reference runtime/voice-library/example-voice.wav]
                                   [--out runtime/logs/verify]
"""
from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

import httpx

RU_TEXT = "Привет! Это проверка клонированного голоса. Синтез выполняется локально."
EN_TEXT = "Hello! This is a cloned voice speaking English. The synthesis runs locally."


def timed(label: str, fn):
    started = time.perf_counter()
    try:
        value = fn()
        ms = int((time.perf_counter() - started) * 1000)
        print(f"[ok]   {label}: {ms} ms")
        return True, ms, value
    except Exception as exc:  # noqa: BLE001 - report and continue
        ms = int((time.perf_counter() - started) * 1000)
        print(f"[FAIL] {label}: {ms} ms -> {exc}")
        return False, ms, None


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="http://127.0.0.1:8080")
    parser.add_argument("--voice", default="example-voice")
    parser.add_argument("--reference", default="runtime/voice-library/example-voice.wav")
    parser.add_argument("--out", default="runtime/logs/verify")
    args = parser.parse_args()
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    results: dict[str, dict] = {}
    client = httpx.Client(base_url=args.base, timeout=600.0)

    ok, ms, health = timed("health", lambda: client.get("/health").json())
    results["health"] = {"ok": ok, "ms": ms, "value": health}

    ok, ms, caps = timed("capabilities", lambda: client.get("/v1/capabilities").json())
    results["capabilities"] = {"ok": ok, "ms": ms}
    if caps:
        print("       gpu:", caps.get("gpu"))
        print("       tts models:", [m["id"] for m in caps.get("tts", {}).get("models", [])])

    # --- ASR on the reference recording -------------------------------------
    ref = Path(args.reference)
    if ref.is_file():
        def asr():
            with ref.open("rb") as fh:
                return client.post(
                    "/v1/audio/transcriptions",
                    files={"file": (ref.name, fh, "audio/wav")},
                    data={"response_format": "json"},
                ).json()

        ok, ms, data = timed("asr (reference recording)", asr)
        results["asr"] = {"ok": ok, "ms": ms, "text": (data or {}).get("text", "")[:200]}
        print("       text:", (data or {}).get("text", "")[:120])
    else:
        print(f"[skip] asr: reference not found at {ref}")

    # --- built-in TTS -------------------------------------------------------
    def speech(model: str, text: str, language: str | None = None):
        payload = {"model": model, "input": text, "response_format": "wav"}
        if language:
            payload["language"] = language
        resp = client.post("/v1/audio/speech", json=payload)
        resp.raise_for_status()
        return resp.content

    ok, ms, wav = timed("tts kokoro (en)", lambda: speech("kokoro", "This is the built-in English voice."))
    results["tts_kokoro"] = {"ok": ok, "ms": ms, "bytes": len(wav) if wav else 0}
    if wav:
        (out_dir / "tts-kokoro-en.wav").write_bytes(wav)

    # No built-in Russian voice exists in this backend: without a profile the
    # service must explain that instead of synthesizing gibberish.
    def ru_without_profile():
        resp = client.post(
            "/v1/audio/speech",
            json={"model": "auto", "input": "Это проверка русского голоса."},
        )
        if resp.status_code != 400 or "voice profile" not in resp.text:
            resp.raise_for_status()
            raise AssertionError(
                f"expected a clear 400 about a missing voice profile, got {resp.status_code}: {resp.text[:200]}"
            )
        return resp.text

    ok, ms, message = timed("ru tts without a profile explains the requirement", ru_without_profile)
    results["tts_ru_without_profile"] = {"ok": ok, "ms": ms, "message": (message or "")[:200]}

    # --- cloned voice: preview (RU) ----------------------------------------
    def preview():
        resp = client.post(f"/v1/voices/{args.voice}/preview")
        resp.raise_for_status()
        return resp.content, resp.headers.get("X-Generation-Ms")

    ok, ms, value = timed("clone preview (ru)", preview)
    results["clone_preview_ru"] = {
        "ok": ok,
        "wall_ms": ms,
        "generation_ms": value[1] if value else None,
        "bytes": len(value[0]) if value else 0,
    }
    if value:
        wav, gen_ms = value
        (out_dir / "clone-preview-ru.wav").write_bytes(wav)

    # --- cloned voice: English through the speech endpoint -----------------
    def clone_en():
        resp = client.post(
            "/v1/audio/speech",
            json={"voice": args.voice, "input": EN_TEXT, "language": "en", "response_format": "wav"},
        )
        resp.raise_for_status()
        return resp.content

    ok, ms, wav = timed("clone speech (en)", clone_en)
    results["clone_speech_en"] = {"ok": ok, "ms": ms, "bytes": len(wav) if wav else 0}
    if wav:
        (out_dir / "clone-speech-en.wav").write_bytes(wav)

    # --- resource snapshot --------------------------------------------------
    ok, ms, status = timed("admin status", lambda: client.get("/admin/status").json())
    if status:
        gpu = status.get("gpu") or {}
        audiocpp = (status.get("backends") or {}).get("audiocpp") or {}
        results["resources"] = {
            "gpu_used_mib": gpu.get("used_mib"),
            "gpu_total_mib": gpu.get("total_mib"),
            "audiocpp_running": audiocpp.get("running"),
            "loaded_models": audiocpp.get("models"),
        }
        print("       gpu used/total MiB:", gpu.get("used_mib"), "/", gpu.get("total_mib"))
        print("       loaded models:", [m.get("id") for m in audiocpp.get("models") or []])

    (out_dir / "verify-results.json").write_text(
        json.dumps(results, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    print(f"\nresults written to {out_dir / 'verify-results.json'}")
    failures = [k for k, v in results.items() if isinstance(v, dict) and v.get("ok") is False]
    print("failures:", failures or "none")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
