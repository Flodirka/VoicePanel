"""Reproducible VoicePanel benchmark: ASR, correction, translation, TTS and
end-to-end latency with process resource sampling.

Run it against a started service (tools/start-service.ps1). Results go to
runtime/logs/bench/<timestamp>-<suite>.json and a short summary is printed.
Inputs are synthetic text suites in tools/bench/cases/ and a local audio
manifest you record yourself; no audio or results belong in Git.

Suites:
  text   correction or translation through the service or straight to an
         OpenAI-compatible llama-server (A/B of candidate models).
  asr    WER/CER, latency and real-time factor from a JSONL manifest.
  e2e    speech end -> processed text: ASR, then correction or translation.
  tts    time to complete WAV and real-time factor.

Examples (PowerShell, from the repository root):
  python tools/bench/run_bench.py text --suite correction --repeat 3
  python tools/bench/run_bench.py text --suite translation --llm-url http://127.0.0.1:8110 --template hy-mt2
  python tools/bench/run_bench.py asr --manifest runtime/bench/asr.jsonl --model qwen3-asr
  python tools/bench/run_bench.py e2e --manifest runtime/bench/asr.jsonl --cold
  python tools/bench/run_bench.py tts --voice my-profile

Every llama-server, audiocpp_server and whisper-server process is sampled and
labelled with its --port, so an agent's own llama-server (Dio) shows up as a
separate row. Add --watch <name> (repeatable) to sample other processes.
"""
from __future__ import annotations

import argparse
import io
import json
import subprocess
import sys
import threading
import time
import wave
from pathlib import Path
from typing import Any, Callable

import httpx

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(ROOT / "service"))

from metrics import cer, chrf, edit_ratio, preserved, script_share, summary, wer  # noqa: E402

CASES = Path(__file__).resolve().parent / "cases"
SERVICE_PROCESSES = ("audiocpp_server", "llama-server", "whisper-server")
LANG_NAMES = {"en": "English", "ru": "Russian"}


# ------------------------------------------------------------------ sampling
class ResourceSampler:
    """Samples RSS/private memory and CPU of the service's engines (and any
    watched process), system available RAM, and GPU memory via nvidia-smi."""

    def __init__(self, names: list[str], interval: float = 0.25) -> None:
        import psutil

        self.psutil = psutil
        self.names = [n.lower() for n in names]
        self.interval = interval
        self.samples: list[dict[str, Any]] = []
        self._stop = threading.Event()
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._procs: dict[int, Any] = {}
        self._gpu_next = 0.0

    def _match(self, proc) -> str | None:
        try:
            name = proc.name().lower()
            if not any(n in name for n in self.names):
                return None
            cmd = proc.cmdline()
            port = next((cmd[i + 1] for i, a in enumerate(cmd[:-1]) if a == "--port"), None)
            if port is None and "--config" in cmd:      # audio.cpp: port is in the generated config name
                port = next((Path(cmd[i + 1]).stem.split("-")[-1] for i, a in enumerate(cmd[:-1])
                             if a == "--config"), None)
            return f"{name}:port{port}" if port else f"{name}:pid{proc.pid}"
        except (self.psutil.NoSuchProcess, self.psutil.AccessDenied):
            return None

    def _gpu(self) -> int | None:
        try:
            out = subprocess.run(
                ["nvidia-smi", "--query-gpu=memory.used", "--format=csv,noheader,nounits"],
                capture_output=True, text=True, timeout=3,
            )
            return int(out.stdout.splitlines()[0].strip()) if out.returncode == 0 else None
        except Exception:
            return None

    def _run(self) -> None:
        ps = self.psutil
        cores = ps.cpu_count() or 1
        while not self._stop.is_set():
            row: dict[str, Any] = {"t": time.monotonic(), "procs": {}}
            for proc in ps.process_iter(["pid"]):
                if proc.pid in self._procs:
                    continue
                label = self._match(proc)
                if label:
                    proc.cpu_percent(None)          # prime the counter
                    self._procs[proc.pid] = (label, proc)
            for pid, (label, proc) in list(self._procs.items()):
                try:
                    mem = proc.memory_info()
                    private = getattr(mem, "private", None) or getattr(mem, "uss", None)
                    row["procs"][label] = {
                        "rss_mib": mem.rss / 1048576,
                        "private_mib": (private / 1048576) if private else None,
                        "cpu_pct_of_machine": proc.cpu_percent(None) / cores,
                    }
                except (ps.NoSuchProcess, ps.AccessDenied):
                    self._procs.pop(pid, None)
            row["available_mib"] = ps.virtual_memory().available / 1048576
            row["system_cpu_pct"] = ps.cpu_percent(None)
            if row["t"] >= self._gpu_next:
                row["gpu_used_mib"] = self._gpu()
                self._gpu_next = row["t"] + 1.0
            self.samples.append(row)
            self._stop.wait(self.interval)

    def __enter__(self) -> "ResourceSampler":
        self._thread.start()
        return self

    def __exit__(self, *exc: Any) -> None:
        self._stop.set()
        self._thread.join(timeout=5)

    def report(self) -> dict[str, Any]:
        per: dict[str, dict[str, list[float]]] = {}
        for row in self.samples:
            for label, values in row["procs"].items():
                bucket = per.setdefault(label, {"rss_mib": [], "cpu": []})
                bucket["rss_mib"].append(values["rss_mib"])
                bucket["cpu"].append(values["cpu_pct_of_machine"])
        gpu = [r["gpu_used_mib"] for r in self.samples if r.get("gpu_used_mib") is not None]
        avail = [r["available_mib"] for r in self.samples]
        return {
            "processes": {
                label: {
                    "rss_peak_mib": round(max(v["rss_mib"]), 1),
                    "rss_last_mib": round(v["rss_mib"][-1], 1),
                    "cpu_mean_pct_of_machine": round(sum(v["cpu"]) / len(v["cpu"]), 1),
                    "cpu_peak_pct_of_machine": round(max(v["cpu"]), 1),
                }
                for label, v in per.items() if v["rss_mib"]
            },
            "system_available_min_mib": round(min(avail), 1) if avail else None,
            "gpu_used_peak_mib": max(gpu) if gpu else None,
            "gpu_used_min_mib": min(gpu) if gpu else None,
            "samples": len(self.samples),
        }


# ------------------------------------------------------------------ helpers
def load_jsonl(path: Path) -> list[dict[str, Any]]:
    rows = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip() and not line.lstrip().startswith("#"):
            rows.append(json.loads(line))
    return rows


def timed(fn: Callable[[], Any]) -> tuple[float, Any]:
    started = time.perf_counter()
    value = fn()
    return (time.perf_counter() - started) * 1000.0, value


def wav_duration_s(data: bytes) -> float:
    with wave.open(io.BytesIO(data), "rb") as w:
        return w.getnframes() / float(w.getframerate())


def crop_wav(data: bytes, seconds: float) -> bytes:
    with wave.open(io.BytesIO(data), "rb") as w:
        params = w.getparams()
        frames = w.readframes(min(w.getnframes(), int(seconds * w.getframerate())))
    out = io.BytesIO()
    with wave.open(out, "wb") as w:
        w.setparams(params)
        w.writeframes(frames)
    return out.getvalue()


def parse_sampling(text: str | None) -> dict[str, Any]:
    """'temperature=0.7,top_p=0.6' or a JSON object; default greedy."""
    if not text:
        return {"temperature": 0.0}
    if text.lstrip().startswith("{"):
        return json.loads(text)
    out: dict[str, Any] = {}
    for item in text.split(","):
        key, _, value = item.partition("=")
        out[key.strip()] = float(value) if any(c in value for c in ".e") else int(value)
    return out


def has_cyrillic(text: str) -> bool:
    return any("Ѐ" <= c <= "ӿ" for c in text)


# ------------------------------------------------------------- text backends
class ServiceText:
    """POST /v1/text/process on the running service (production prompts)."""

    def __init__(self, client: httpx.Client, profile: str, temperature: float | None) -> None:
        self.client, self.profile, self.temperature = client, profile, temperature

    def run(self, text: str, mode: str, context: str | None) -> str:
        body: dict[str, Any] = {"text": text, "mode": mode, "profile": self.profile}
        if context:
            body["context"] = context
        if self.temperature is not None:
            body["temperature"] = self.temperature
        resp = self.client.post("/v1/text/process", json=body)
        resp.raise_for_status()
        return resp.json()["result"]


class DirectLlm:
    """An OpenAI-compatible llama-server started by hand with a candidate model.

    Templates:
      voicepanel  the service's own system prompt (processing.build_messages)
      hy-mt2      Tencent Hy-MT2 user-only prompt, glossary as reference pairs
      custom      --user-template file with {text}, {target_lang}, {context}
    """

    def __init__(self, url: str, template: str, sampling: dict[str, Any], user_template: str | None) -> None:
        self.client = httpx.Client(base_url=url.rstrip("/"), timeout=600, trust_env=False)
        self.template, self.sampling, self.user_template = template, sampling, user_template

    def messages(self, text: str, mode: str, context: str | None, glossary: list[list[str]] | None):
        if self.template == "voicepanel":
            from voiceservice.processing import build_messages

            return build_messages(text, mode, context=context)
        target = LANG_NAMES["en" if mode.endswith("_en") else "ru"]
        if self.template == "hy-mt2":
            prompt = ""
            if glossary:
                prompt += "Reference the following translations:\n" + "\n".join(
                    f"{src} translates to {dst}" for src, dst in glossary) + "\n\n"
            prompt += (f"Translate the following text into {target}. Note that you should only "
                       f"output the translated result without any additional explanation:\n\n{text}")
            return [{"role": "user", "content": prompt}]
        template = self.user_template or "{text}"
        return [{"role": "user", "content": template.format(text=text, target_lang=target, context=context or "")}]

    def run(self, text: str, mode: str, context: str | None, glossary=None) -> str:
        payload = {"messages": self.messages(text, mode, context, glossary), "stream": False,
                   "max_tokens": len(text) // 2 + 128, **self.sampling}
        resp = self.client.post("/v1/chat/completions", json=payload)
        resp.raise_for_status()
        return resp.json()["choices"][0]["message"]["content"].strip()


# -------------------------------------------------------------------- suites
def suite_text(args, client: httpx.Client) -> dict[str, Any]:
    cases = load_jsonl(CASES / f"{args.suite}.jsonl")
    if args.llm_url:
        sampling = parse_sampling(args.sampling)
        backend: Any = DirectLlm(args.llm_url, args.template, sampling, _read(args.user_template))
    else:
        backend = ServiceText(client, args.profile, args.temperature)
    rows, latencies = [], []
    for rep in range(args.repeat):
        for case in cases:
            if args.suite == "correction":
                mode = "correct_ru" if case["lang"] == "ru" else "correct_en"
            else:
                mode = f"translate_{case['source']}_{case['target']}"
            context = case.get("context") or args.context
            call = (lambda: backend.run(case["input"], mode, context, case.get("glossary"))) \
                if isinstance(backend, DirectLlm) else (lambda: backend.run(case["input"], mode, context))
            ms, output = timed(call)
            if rep > 0 or not args.discard_first:
                latencies.append(ms)
            kept, total, missing = preserved(case.get("protect", []), output)
            row = {"id": case["id"], "rep": rep, "ms": round(ms, 1), "output": output,
                   "protected_kept": kept, "protected_total": total, "missing": missing}
            if args.suite == "correction":
                row["changed_unchanged_input"] = case["kind"] == "unchanged" and output.strip() != case["input"].strip()
                row["edit_vs_expected"] = round(edit_ratio(case["expected"], output), 3)
                row["edit_vs_input"] = round(edit_ratio(case["input"], output), 3)
                row["chrf"] = round(chrf(case["expected"], output), 1)
                row["wrong_script"] = script_share(output, case["lang"]) < 0.5 and case["lang"] == "ru"
            else:
                row["chrf"] = round(chrf(case["reference"], output), 1)
                row["target_script_share"] = round(script_share(output, case["target"]), 2)
                row["required_missing"] = [t for t in case.get("require", []) if t.lower() not in output.lower()]
            rows.append(row)
    final = [r for r in rows if r["rep"] == args.repeat - 1]
    out: dict[str, Any] = {"latency_ms": summary(latencies), "cases": rows}
    kept = sum(r["protected_kept"] for r in final)
    total = sum(r["protected_total"] for r in final) or 1
    out["protected_token_rate"] = round(kept / total, 3)
    out["chrf_mean"] = round(sum(r["chrf"] for r in final) / len(final), 1)
    if args.suite == "correction":
        unchanged = [r for r in final if any(c["id"] == r["id"] and c["kind"] == "unchanged" for c in cases)]
        out["unchanged_kept_rate"] = round(1 - sum(r["changed_unchanged_input"] for r in unchanged) / max(1, len(unchanged)), 3)
        out["edit_vs_expected_mean"] = round(sum(r["edit_vs_expected"] for r in final) / len(final), 3)
        out["wrong_script_count"] = sum(r["wrong_script"] for r in final)
    else:
        out["required_terms_missing"] = sum(len(r["required_missing"]) for r in final)
    return out


def suite_asr(args, client: httpx.Client) -> dict[str, Any]:
    manifest = load_jsonl(Path(args.manifest))
    base = Path(args.manifest).parent
    rows, lat, rtf = [], [], []
    for rep in range(args.repeat):
        for item in manifest:
            data = (base / item["audio"]).read_bytes()
            duration = wav_duration_s(data)
            ms, text = timed(lambda: _transcribe(client, data, args.model, args.prompt))
            if rep > 0 or not args.discard_first:
                lat.append(ms)
                rtf.append(ms / 1000.0 / duration)
            row = {"audio": item["audio"], "rep": rep, "ms": round(ms, 1), "duration_s": round(duration, 2),
                   "text": text, "wer": round(wer(item["text"], text), 4), "cer": round(cer(item["text"], text), 4),
                   "tags": item.get("tags", [])}
            if args.prefix_step and rep == 0:
                row["partials"] = _partials(client, data, duration, args, text)
            rows.append(row)
    final = [r for r in rows if r["rep"] == args.repeat - 1]
    by_tag: dict[str, list[dict[str, Any]]] = {}
    for r in final:
        for tag in r["tags"] or ["untagged"]:
            by_tag.setdefault(tag, []).append(r)
    out = {
        "latency_ms": summary(lat),
        "rtf": summary(rtf),
        "wer_mean": round(sum(r["wer"] for r in final) / len(final), 4),
        "cer_mean": round(sum(r["cer"] for r in final) / len(final), 4),
        "by_tag": {t: {"wer": round(sum(r["wer"] for r in v) / len(v), 4),
                       "cer": round(sum(r["cer"] for r in v) / len(v), 4), "n": len(v)} for t, v in by_tag.items()},
        "cases": rows,
    }
    firsts = [p["first_partial_s"] for r in rows if r.get("partials") for p in [r["partials"]] if p["first_partial_s"]]
    if firsts:
        out["time_to_first_partial_s"] = summary(firsts)
        out["partial_word_stability"] = round(
            sum(r["partials"]["stability"] for r in rows if r.get("partials")) /
            len([r for r in rows if r.get("partials")]), 3)
    return out


def _transcribe(client: httpx.Client, wav: bytes, model: str | None, prompt: str | None) -> str:
    data = {"response_format": "json"}
    if model:
        data["model"] = model
    if prompt:
        data["prompt"] = prompt
    resp = client.post("/v1/audio/transcriptions", files={"file": ("a.wav", wav, "audio/wav")}, data=data)
    resp.raise_for_status()
    return resp.json().get("text", "")


def _partials(client, data: bytes, duration: float, args, final_text: str) -> dict[str, Any]:
    """Emulates the panel's periodic speculation: re-recognise growing prefixes.
    Stability = share of each partial's words that survive into the final."""
    final_words = final_text.lower().split()
    first, stabilities, t = None, [], args.prefix_step
    while t < duration:
        ms, text = timed(lambda: _transcribe(client, crop_wav(data, t), args.model, args.prompt))
        words = text.lower().split()
        if words and first is None:
            first = t + ms / 1000.0
        if words:
            same = 0
            for a, b in zip(words[:-1], final_words):   # the last partial word is usually cut
                if a != b:
                    break
                same += 1
            stabilities.append(same / max(1, len(words) - 1))
        t += args.prefix_step
    return {"first_partial_s": first, "stability": sum(stabilities) / len(stabilities) if stabilities else 1.0}


def suite_e2e(args, client: httpx.Client) -> dict[str, Any]:
    """Speech end -> processed text, as the panel sees it after the VAD pause:
    final ASR on the utterance, then correction or translation."""
    manifest = load_jsonl(Path(args.manifest))
    base = Path(args.manifest).parent
    rows, totals, asr_ms, llm_ms = [], [], [], []
    for rep in range(args.repeat):
        for item in manifest:
            if args.cold:
                for name in ("audiocpp", "llama"):
                    client.post(f"/admin/backends/{name}/unload")
            elif args.warmup:
                client.post("/v1/warmup", json={"mode": "auto_translate" if args.translate else "auto_correct",
                                                "context": args.context})
                time.sleep(args.warmup_wait)
            data = (base / item["audio"]).read_bytes()
            t_asr, text = timed(lambda: _transcribe(client, data, args.model, args.prompt))
            mode = (("translate_ru_en" if has_cyrillic(text) else "translate_en_ru") if args.translate
                    else ("correct_ru" if has_cyrillic(text) else "correct_en"))
            t_llm, result = timed(lambda: ServiceText(client, args.profile, args.temperature).run(text, mode, args.context)) \
                if text.strip() else (0.0, "")
            if rep > 0 or not args.discard_first or args.cold:
                totals.append(t_asr + t_llm); asr_ms.append(t_asr); llm_ms.append(t_llm)
            rows.append({"audio": item["audio"], "rep": rep, "asr_ms": round(t_asr, 1), "text_ms": round(t_llm, 1),
                         "transcript": text, "result": result, "wer": round(wer(item["text"], text), 4)})
    return {"speech_end_to_text_ms": summary(totals), "asr_ms": summary(asr_ms), "text_ms": summary(llm_ms),
            "note": "Excludes the VAD end-of-speech wait (sessions.silence_frames x 32 ms) and insertion.",
            "cases": rows}


def suite_tts(args, client: httpx.Client) -> dict[str, Any]:
    texts = {
        "en_short": "This is a short English sentence.",
        "en_long": "VoicePanel reads typed text aloud. It splits long text into sentences and synthesizes "
                   "them ahead of playback, so the next sentence is ready when the current one ends.",
        "ru_short": "Это короткое русское предложение.",
        "ru_long": "Голосовая панель читает набранный текст вслух. Длинный текст делится на предложения, "
                   "и следующее предложение готово к концу текущего.",
    }
    rows, out = [], {}
    for rep in range(args.repeat):
        for key, text in texts.items():
            body: dict[str, Any] = {"input": text, "response_format": "wav"}
            if key.startswith("ru") or args.voice:
                if not args.voice:
                    continue    # Russian needs a voice profile
                body["voice"] = args.voice
            else:
                body["model"] = args.tts_model
            ms, resp = timed(lambda: client.post("/v1/audio/speech", json=body))
            if resp.status_code != 200:
                rows.append({"case": key, "rep": rep, "error": resp.text[:200]})
                continue
            audio_s = wav_duration_s(resp.content)
            rows.append({"case": key, "rep": rep, "ms": round(ms, 1), "audio_s": round(audio_s, 2),
                         "speed_x_realtime": round(audio_s / (ms / 1000.0), 2)})
    for key in texts:
        vals = [r["ms"] for r in rows if r.get("case") == key and "ms" in r and (r["rep"] > 0 or not args.discard_first)]
        if vals:
            out[key] = {"time_to_audio_ms": summary(vals),
                        "speed_x_realtime": round(sum(r["speed_x_realtime"] for r in rows if r.get("case") == key and "ms" in r)
                                                  / len([r for r in rows if r.get("case") == key and "ms" in r]), 2)}
    out["note"] = ("Offline synthesis: time to audio equals time to the complete WAV of one sentence. "
                   "The panel plays sentence 1 while sentence 2 is synthesized.")
    out["cases"] = rows
    return out


def _read(path: str | None) -> str | None:
    return Path(path).read_text(encoding="utf-8") if path else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("suite_name", choices=["text", "asr", "e2e", "tts"])
    parser.add_argument("--base", default="http://127.0.0.1:8080")
    parser.add_argument("--suite", choices=["correction", "translation"], default="correction")
    parser.add_argument("--repeat", type=int, default=3)
    parser.add_argument("--discard-first", action=argparse.BooleanOptionalAction, default=True,
                        help="exclude the first (cold) repetition from latency statistics")
    parser.add_argument("--profile", default="general")
    parser.add_argument("--temperature", type=float, default=None)
    parser.add_argument("--context", default=None, help="context document text for text and e2e suites")
    parser.add_argument("--context-file", default=None)
    parser.add_argument("--llm-url", default=None, help="OpenAI-compatible llama-server to test directly")
    parser.add_argument("--template", choices=["voicepanel", "hy-mt2", "custom"], default="voicepanel")
    parser.add_argument("--user-template", default=None, help="file with {text}/{target_lang}/{context}")
    parser.add_argument("--sampling", default=None, help="e.g. temperature=0.7,top_p=0.6,top_k=20 (or JSON)")
    parser.add_argument("--manifest", default=None, help="JSONL: {\"audio\": \"x.wav\", \"text\": \"...\", \"tags\": [..]}")
    parser.add_argument("--model", default=None, help="ASR model id")
    parser.add_argument("--prompt", default=None, help="ASR context prompt")
    parser.add_argument("--prompt-file", default=None, help="ASR context prompt from a file (the context document)")
    parser.add_argument("--prefix-step", type=float, default=0.0, help="seconds; >0 measures partial results")
    parser.add_argument("--translate", action="store_true", help="e2e: translate instead of correct")
    parser.add_argument("--cold", action="store_true", help="e2e: unload engines before every utterance")
    parser.add_argument("--warmup", action="store_true", help="e2e: call /v1/warmup before every utterance")
    parser.add_argument("--warmup-wait", type=float, default=1.5, help="seconds between warm-up and speech end")
    parser.add_argument("--voice", default=None, help="tts: voice profile id (needed for Russian)")
    parser.add_argument("--tts-model", default="kokoro")
    parser.add_argument("--watch", action="append", default=[], help="extra process name to sample")
    parser.add_argument("--label", default="", help="free-form label stored with the results")
    parser.add_argument("--out", default=str(ROOT / "runtime" / "logs" / "bench"))
    args = parser.parse_args()
    if args.context_file:
        args.context = Path(args.context_file).read_text(encoding="utf-8")
    if args.prompt_file:
        args.prompt = Path(args.prompt_file).read_text(encoding="utf-8")
    if args.suite_name in ("asr", "e2e") and not args.manifest:
        parser.error("--manifest is required for asr and e2e")

    client = httpx.Client(base_url=args.base, timeout=600, trust_env=False)
    suites = {"text": suite_text, "asr": suite_asr, "e2e": suite_e2e, "tts": suite_tts}
    with ResourceSampler(list(SERVICE_PROCESSES) + args.watch) as sampler:
        started = time.time()
        result = suites[args.suite_name](args, client)
    result["resources"] = sampler.report()
    result["meta"] = {
        "suite": args.suite_name, "text_suite": args.suite if args.suite_name == "text" else None,
        "label": args.label, "started": time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime(started)),
        "wall_s": round(time.time() - started, 1),
        "args": {k: v for k, v in vars(args).items() if k not in ("context", "prompt")},
        "platform": sys.platform, "python": sys.version.split()[0],
    }
    try:
        result["meta"]["service_status"] = client.get("/admin/status").json()
    except Exception:
        pass
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    name = f"{time.strftime('%Y%m%d-%H%M%S')}-{args.suite_name}{'-' + args.suite if args.suite_name == 'text' else ''}.json"
    (out_dir / name).write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    brief = {k: v for k, v in result.items() if k not in ("cases", "meta")}
    print(json.dumps(brief, indent=2, ensure_ascii=False))
    print(f"\nfull results: {out_dir / name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
