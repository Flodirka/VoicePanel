"""Workflow-level verification: the same chain the panel runs for dictation.

A recorded WAV goes through POST /v1/audio/transcriptions (what
DictationController sends per utterance), the transcript is cleaned and split
into complete sentences with the service's own processing helpers (what
TextProcessor does), and each sentence goes through POST /v1/text/process with
the user dictionary — so the deterministic pre-pass and the skip-if-clean gate
are exercised exactly as in the running application, on real ASR output.

This is deliberately NOT a raw-text model benchmark: every text the corrector
sees comes out of the recogniser.

Usage:
    python tools/verify_workflow.py --base http://127.0.0.1:8080 \
        --audio runtime/bench/mixed-16k.wav \
        --dictionary runtime/bench/workflow-dictionary.txt \
        [--context panel/resources/default-context.md] \
        [--asr-model nemotron] [--out runtime/logs/workflow]
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

import httpx

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "service"))

from voiceservice.processing import clean_transcript, complete_sentence_prefix  # noqa: E402


def has_cyrillic(text: str) -> bool:
    return any("\u0400" <= ch <= "\u04ff" for ch in text)


def load_terms(path: Path | None) -> list[str]:
    if path is None or not path.is_file():
        return []
    return [
        line.split("#", 1)[0].strip()
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.split("#", 1)[0].strip()
    ]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="http://127.0.0.1:8080")
    parser.add_argument("--audio", required=True, help="recorded WAV to dictate")
    parser.add_argument("--dictionary", default=None, help="one term per line, '#' comments")
    parser.add_argument("--context", default=None, help="context document sent with each request")
    parser.add_argument("--asr-model", default=None)
    parser.add_argument("--out", default="runtime/logs/workflow")
    args = parser.parse_args()

    audio = Path(args.audio)
    if not audio.is_file():
        parser.error(f"audio not found: {audio}")
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)

    terms = load_terms(Path(args.dictionary) if args.dictionary else None)
    context = Path(args.context).read_text(encoding="utf-8") if args.context and Path(args.context).is_file() else None

    client = httpx.Client(base_url=args.base, timeout=900.0)
    results: dict = {"audio": str(audio), "dictionary": terms, "stages": {}}
    failures = 0

    def step(label: str, fn):
        nonlocal failures
        started = time.perf_counter()
        try:
            value = fn()
            ms = int((time.perf_counter() - started) * 1000)
            print(f"[ok]   {label}: {ms} ms")
            results["stages"][label] = {"ok": True, "ms": ms, "value": value}
            return value
        except Exception as exc:  # noqa: BLE001
            ms = int((time.perf_counter() - started) * 1000)
            print(f"[FAIL] {label}: {ms} ms -> {exc}")
            results["stages"][label] = {"ok": False, "ms": ms, "error": str(exc)}
            failures += 1
            return None

    # Session start: the panel posts this before the first utterance.
    def warmup():
        resp = client.post("/v1/warmup", json={
            "asr": True, "llm": True, "mode": "auto_correct",
            "context": context, "dictionary": terms or None,
        })
        resp.raise_for_status()
        return resp.json()

    step("warmup", warmup)

    # Utterance: exactly what DictationController sends per finished segment.
    def asr():
        data = {"response_format": "json"}
        if args.asr_model:
            data["model"] = args.asr_model
        with audio.open("rb") as fh:
            resp = client.post("/v1/audio/transcriptions", files={"file": (audio.name, fh, "audio/wav")}, data=data)
        resp.raise_for_status()
        return resp.json()

    asr_data = step("asr (audio -> transcript)", asr)
    raw_text = (asr_data or {}).get("text", "")
    transcript = clean_transcript(raw_text)
    results["asr_raw"] = raw_text
    results["asr_clean"] = transcript
    print(f"       transcript: {transcript[:200]}")

    (out_dir / "workflow-results.json").parent.mkdir(parents=True, exist_ok=True)
    if not transcript.strip():
        print("[FAIL] asr produced no text; nothing to correct")
        results["failures"] = failures + 1
        (out_dir / "workflow-results.json").write_text(json.dumps(results, indent=2, ensure_ascii=False), encoding="utf-8")
        return 1

    # Correction: one request per complete sentence, auto language per
    # sentence — what TextProcessor::dispatchPending does in the panel.
    sentences: list[str] = []
    rest = transcript
    while rest.strip():
        complete, rest = complete_sentence_prefix(rest)
        if complete:
            sentences.extend(s for s in complete.split("\n") if s.strip())
        else:
            break   # no sentence-final punctuation left; the tail stays verbatim
    if not sentences:
        sentences = [transcript.strip()]

    per_sentence = []
    skipped = 0
    for i, sentence in enumerate(sentences):
        mode = "correct_ru" if has_cyrillic(sentence) else "correct_en"

        def process(sentence=sentence, mode=mode):
            resp = client.post("/v1/text/process", json={
                "text": sentence, "mode": mode, "dictionary": terms or None,
                "context": context, "spelling": [],
            })
            resp.raise_for_status()
            return resp.json()

        body = step(f"text/process sentence {i + 1} ({mode})", process)
        if body is None:
            continue
        if body.get("model") is None:
            skipped += 1
        per_sentence.append({
            "input": sentence, "mode": mode, "result": body.get("result"),
            "model": body.get("model"), "notes": body.get("notes"),
            "duration_ms": body.get("duration_ms"),
        })
        print(f"       {sentence[:60]!r} -> {str(body.get('result'))[:60]!r}"
              f"  [model={body.get('model')}, {body.get('duration_ms')} ms]")
        if body.get("notes"):
            print(f"       notes: {body['notes']}")

    results["sentences"] = per_sentence
    results["llm_skipped_clean"] = skipped
    results["llm_called"] = len(per_sentence) - skipped
    results["failures"] = failures

    (out_dir / "workflow-results.json").write_text(
        json.dumps(results, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    print(f"\nsentences: {len(per_sentence)}, model called: {len(per_sentence) - skipped}, "
          f"skipped as clean (no LLM call): {skipped}")
    print(f"results written to {out_dir / 'workflow-results.json'}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())