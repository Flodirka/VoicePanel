# Performance and model audit

Date: 2026-11-03. Scope: the speech pipeline from microphone to inserted text, the text models, speech synthesis, and process lifecycles. Target: Windows 11, RTX 4060 Laptop (8 GB), 32 GB RAM, next to a local agent (Dio) that runs a large MoE model through llama.cpp.

## Summary

The pipeline design is sound: VAD-gated utterances, speculative recognition at pauses, ordered finals, sentence-level text processing, and separate engine processes. The audit found no reason to replace a default model now. Every candidate either lacks evidence on Russian, lacks a feature that VoicePanel uses (the ASR context prompt), or needs a local quality check that a cloud container cannot run.

The implemented changes remove fixed overhead and worst cases:

1. The first sentence after an idle period no longer pays the model cold start. The panel asks the service to load the ASR model and prefill the text model's prompt when dictation starts (`POST /v1/warmup`).
2. Switching between Russian and English sentences no longer re-evaluates the context document. The stable parts of the system prompt now come first.
3. The llama-server prompt cache can no longer grow to its 8192 MiB default. It is bounded to 512 MiB.
4. A runaway generation cannot block the dictation queue. Requests without `max_tokens` get a cap, and a capped result returns the original text.
5. Each gateway-to-engine call is about 13 ms faster at p50 (measured on Linux, see below).
6. Long utterances cost less speculative recognition (20 s utterance: 74 s → 35 s of re-recognized audio, simulated).
7. The WebSocket dictation route now produces partials, keeps listening while it recognizes, and keeps finals in order.

The 20 % end-to-end target is expected for the first sentence of a session after idle. It is not expected for steady-state sentences from these changes alone. The largest remaining levers are model-level and need the laptop: a dedicated translation model (Hy-MT2-1.8B), KV cache quantization, the VAD end-of-speech wait, and thread allocation next to Dio. The benchmark harness in `tools/bench/` measures each of them.

### Evidence labels

| Label | Meaning |
|---|---|
| **code** | Verified by reading the source in this repository |
| **test** | Executed in this audit (Linux container, model-free) |
| **linux** | Measured in the cloud container: 4-core Intel Xeon 2.1 GHz, 15 GB RAM, no GPU, no models. Not a Windows or RTX 4060 result |
| **upstream** | Published by a model vendor, runtime project or third party; not reproduced here |
| **computed** | Derived arithmetic from published architecture or configuration values |
| **expected** | A prediction; needs the local benchmark |

No model inference ran in this audit. Hugging Face, arXiv and most model hosts were blocked by the container's network policy, so several model cards could not be read directly. Those gaps are marked.

## 1. Current architecture

**code.** Two processes own the work. The Qt panel owns Windows interaction. The Python service (FastAPI) owns model routing and supervises engine processes.

```text
WASAPI capture thread ── 16 kHz mono float, 64 ms blocks ──▶ GUI thread
  Silero VAD (ONNX, 1 thread) per 32 ms frame
  pre-roll 11 frames ─ segment ─ speculative ASR (every 63 frames, and at 6 quiet frames)
  segment end at 24 quiet frames (0.77 s) or 30 s
        │  multipart WAV over HTTP (QNetworkAccessManager, keep-alive)
        ▼
Python gateway :8080 ──▶ audio.cpp :8101 (CPU)   Qwen3-ASR 0.6B Q8_0, Kokoro 82M Q8_0
                    ──▶ audio.cpp :8104 (CUDA)  Qwen3-TTS 0.6B Base Q8_0, on demand
                    ──▶ llama-server :8103 (CPU) Qwen3-4B-Instruct-2507 Q4_K_M
                    ──▶ whisper-server :8102     optional, timestamped file ASR
        ▲
DictationController orders finals ─▶ TextProcessor: complete sentences only,
one request in flight, later sentences batch ─▶ MainWindow: Quick insert or Review
```

Pipeline facts that shape latency:

- **Utterance end.** A segment ends after `silence_frames` × 32 ms = 0.77 s of silence (`DictationController::processBlock`).
- **Final recognition.** At 6 quiet frames (192 ms) the controller sends a speculative request for the whole segment. If no speech frame arrives before the segment ends, that result becomes the final (`finishSegment`, revision check). The final ASR therefore usually starts 0.58 s before the utterance is declared finished.
- **Ordering.** Finals are delivered in segment order through `finalResults_`. Text processing sends only complete sentences and keeps one request in flight; later sentences wait and go in one batch.
- **Gateway serialization.** One `asyncio.Lock` per engine type (`_asr_lock`, `_tts_lock`). llama-server runs `--parallel 1`.
- **Lifecycles.** audio.cpp loads models lazily and unloads all resident models after `idle_unload_ms` (90 s main, 60 s clone) of server inactivity. The gateway stops a process after `idle_stop_s` (main 1800 s, clone 120 s, llama 900 s, whisper 1800 s) without leases.

## 2. Performance audit

Severity ranks user-visible latency, resource use next to Dio, and correctness.

| # | Finding | Path | Evidence | Severity | Status |
|---|---|---|---|---|---|
| A1 | Cold start on the first sentence after idle: audio.cpp unloads the ASR model after 90 s without activity; llama-server stops after 15 min. The first final then pays model load, and the first correction pays process start and full prompt prefill. | `config.example.yaml`, audio.cpp server docs | code, upstream | High for the first sentence | **Fixed**: `POST /v1/warmup` at session start |
| A2 | The system prompt put the per-request task line before the context document. In auto mode the task changes with each sentence's language, so llama.cpp could reuse only the first rules block and re-evaluated the context document on every RU/EN switch. | `processing.build_messages` | code, test | Medium; grows with the context document | **Fixed**: stable parts first |
| A3 | llama-server b11871 keeps a host-RAM prompt cache with an 8192 MiB default and saves idle slots into it. Different prompts (other modes, `/v1/chat/completions` clients) can grow the process by gigabytes. | llama.cpp server README at b11871 | upstream, code | Medium (memory pressure next to Dio) | **Fixed**: `cache_ram_mib: 512` |
| A4 | No generation limit when the client sends no `max_tokens`. A repetition loop (seen before with `previous`) runs until the 8192-token context fills, for minutes of CPU time, while later sentences wait. | `api/text.py` | code | Medium (rare, severe) | **Fixed**: derived cap and fallback to the original text |
| A5 | Every engine call built a new `httpx.AsyncClient` and the lease made a second client for a health probe. | `process.py`, `audiocpp.py`, `llama.py`, `whisper.py` | linux: p50 15.5 ms / p95 37.7 ms → 2.0 / 2.8 ms per call | Low (×2 per sentence on the critical path) | **Fixed**: pooled client, probe skipped for a healthy own child |
| A6 | A process started by `/admin/backends/{name}/load` kept its old idle timestamp; the idle watcher could stop it within 15 s. | `ManagedProcess._spawn` | code, test | Low (correctness) | **Fixed** |
| A7 | Periodic speculation re-recognizes the whole segment every 2 s. Work grows quadratically with utterance length and competes with correction of earlier sentences. | `DictationController::maybeSpeculate`, `vad.py` | Simulation of the panel's gating rule (one request in flight): 20 s utterance at 5× real time: 74 s of audio; 30 s: 120 s. At 2× real time the in-flight gate already limits it | Low–medium (long utterances only) | **Fixed**: interval grows to half the last request (35 s / 55 s); pause speculation unchanged |
| A8 | WebSocket route: `Segment.confirmed` was never set, so speculative partials never ran; the async preview callback was never awaited; finalization blocked audio intake. | `api/sessions.py`, `vad.py` | code, test (fails on the old code) | Medium for WebSocket clients; the panel does not use this route | **Fixed** |
| A9 | Thread oversubscription: audio.cpp and llama-server each use 8 threads. ASR of utterance N+1 overlaps correction of utterance N, and Dio's llama.cpp may use most cores. ggml thread pools spin-wait (`--poll 50` default) and slow down sharply when preempted. | `config.example.yaml`, llama.cpp README | code, upstream | Potentially high with Dio | **Open**: needs a thread sweep on the laptop (section 9) |
| A10 | The ASR request carries the whole context document as `prompt`, including every speculative request. Qwen3-ASR prefills it each time; cost grows with document size. | `ServiceClient::transcribe` | code; cost not measured | Unknown; low for the 568-byte default | **Open**: measure with `asr --prompt` (section 9) |
| A11 | WASAPI resampling is linear interpolation without a low-pass filter, and each packet edge clamps to its last sample. Energy above 8 kHz aliases into the speech band at 16 kHz. | `WasapiCapture.cpp::resampleAudio` | code | Unknown quality effect on ASR and VAD | **Open**: request 16 kHz with `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM \| AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY`, then measure WER; Windows-only, not testable here |
| A12 | `_tts_lock` is shared by the CPU Kokoro process and the GPU clone helper; cloned synthesis blocks built-in English speech. `_asr_lock` also covers whisper. | `backends/__init__.py`, `api/openai.py` | code | Low | Open |
| A13 | The KV cache is f16 at `ctx_size` 8192: Qwen3-4B (36 layers, 8 KV heads, head dim 128) needs 144 KiB per token, 1.125 GiB in total. | llama.cpp defaults | computed | Medium (RAM) | **Configurable**: `cache_type_k/v: q8_0` saves about 0.53 GiB; quality check pending |
| A14 | Final segments include 0.77 s of trailing silence and 0.35 s of pre-roll; the speculative final includes 0.19 s. | `DictationController` | code | Low (encoder cost only) | Open; trimming risks cutting soft word endings |
| A15 | The runtime download group fetches `chatterbox-q8_0.gguf`, `ggml-base.bin` and `ggml-large-v3-turbo-q5_0.bin`, which the sample configuration does not use. | `tools/downloads.lock.json` | code | Low (disk and download only) | Open; owner decision |
| A16 | `chat_completions` streaming returns the response after leaving the LLM semaphore, so streamed requests are not counted. | `api/openai.py` | code | Low | Open |

Checked and found adequate: VAD runs on one ONNX thread per stream; capture runs on its own thread and is never blocked by HTTP; the panel's `QNetworkAccessManager` reuses connections; finals and insertions keep order; failed text processing keeps the original text; edit protection (`userEditedResult_`, revision checks) is intact. Speech synthesis already pipelines two sentences ahead of playback.

### Latency model of one dictated sentence

**code + expected.** After the speaker stops: the VAD decides after 0.77 s. The final ASR started at 0.19 s, so recognition adds `max(0, ASR time − 0.58 s)`. Correction follows (recorded locally as about 1.3 s per sentence with Qwen3-4B; historical, not verified here). Insertion is a clipboard paste. For a typical 3–5 s utterance at about 5× real time, ASR takes 0.6–1.0 s, so the speech-end-to-text time is roughly 0.8–1.2 s + 1.3 s ≈ 2.1–2.5 s. Text processing is the largest part. Translation mode is similar.

## 3. Model research

### A. Speech recognition

| Model | Runtime on Windows | RU / EN accuracy | Streaming | Context prompt | License | Notes |
|---|---|---|---|---|---|---|
| **Qwen3-ASR-0.6B Q8_0** (current) | audio.cpp `qwen3_asr` | RU: no official per-language figure retrieved. Third-party Apple Silicon run, FLEURS RU WER 10.52 (8-bit) — different runtime and quantization (upstream) | Buffered windows | **Yes** (`prompt`) | Apache-2.0 | Mixed RU/EN in one model; current default |
| **Parakeet-TDT-0.6B-v3** | audio.cpp `parakeet_tdt` (community model; CPU and CUDA verified by audio.cpp) | FLEURS RU WER 5.51 (NVIDIA card, via search), 5.91 (Open ASR Leaderboard setup) (upstream) | Buffered re-encoding of bounded windows, "not native cache-aware streaming" | **No** option in audio.cpp's table | CC-BY-4.0 | 25 European languages, auto language detection; best published Russian number of the candidates |
| **Nemotron 3.5 ASR Streaming 0.6B** | audio.cpp `nemotron_asr` (core, streaming); NVIDIA NeMo-Speech.cpp v0.3.0 (2026-10-30) | FLEURS RU WER 12.61 % (Q8_0, transcribe.cpp port); LibriSpeech test-clean EN 3.05 % (Q8_0) (upstream) | **Cache-aware** RNN-T, trained lookahead 0/240/480/1040 ms | Prompt-conditioned on a language code, not free text | OpenMDW-1.1 (license text not read here) | CPU: 9.6–11.6× real time on a Ryzen 7 PRO 4750U (transcribe.cpp, upstream). The port needs an explicit language; audio.cpp lists "100+ ASR prompt codes incl. auto" |
| Whisper large-v3-turbo Q5_0 | whisper.cpp (already supported) | Strong multilingual baseline | No | `prompt` (initial prompt) | MIT | Larger and slower on CPU than the 0.6B models; useful as a file-transcription reference |

**Verdict.** Keep Qwen3-ASR as the default. It is the only candidate that takes the free-text context prompt VoicePanel sends (terminology from the context document). Parakeet v3 is the best-evidenced Russian candidate and needs only a model entry in the existing audio.cpp process; test it as an optional model with the `asr` suite on your own mixed RU/EN technical recordings. Nemotron 3.5 has the only true streaming path, but its published Russian WER is worse than Parakeet's, and its accuracy on mixed-language utterances is not documented in what I could read.

**Is true streaming worth a new path?** Not now. With pause speculation, final recognition already starts 0.58 s before the VAD declares the utterance finished, so for utterances up to about 3 s at 5× real time the recognition is hidden. Streaming would save `ASR time − 0.58 s` on longer utterances (about 0.4 s at 5 s, 1.4 s at 10 s; expected). That gain is real for long utterances but needs: a model configured with `mode: streaming`, a chunked-upload proxy in the gateway (`/v1/audio/transcriptions/live`), and a new panel transport. The model lock is held for the whole live request. NeMo-Speech.cpp as a separate runtime is not justified: audio.cpp already serves both NVIDIA models, and NeMo-Speech.cpp calls itself an early release whose API may change.

**audio.cpp version.** The lock pins v0.9.2. v0.9.3 (2026-10-26) adds LoRA, `perf_mode` presets, a Qwen3-TTS optimized streaming mode and lower packed-weight loading RAM (opt-in). Its CPU work in the release notes targets NEON (ARM), not x86. Qwen3-ASR performance controls landed on `main` after v0.9.3 and target CUDA ("can slow short requests"). No x86 CPU gain for the current models is documented. Upgrade only after an A/B with `asr` and `tts` suites, and record new SHA-256 values in the lock from your own verified download.

### B. Text correction

| Model | Runtime | Evidence on this task | RAM (file) | License |
|---|---|---|---|---|
| **Qwen3-4B-Instruct-2507 Q4_K_M** (current) | llama.cpp | Local history: acceptable correction, about 1.3 s per sentence (not verified here) | 2.50 GB file + KV | Apache-2.0 |
| Qwen3.5-0.8B Q4_K_M | llama.cpp | Local history: about 0.5 s per sentence, rewrites sentences — rejected | ~0.53 GB | Apache-2.0 |
| **loqira/Qwen3.5-0.8B-GEC-KAZ-RUS-ENG** | llama.cpp, if a GGUF exists or after conversion | The Hugging Face listing exists (published days before this audit). The model card could not be read: training data, prompt format, metrics, license and formats are **unknown** | ~0.5–0.9 GB expected | Unknown |

The correction task needs minimal edits and verbatim technical tokens. A dedicated GEC fine-tune of the same 0.8B base could fix the paraphrasing seen with the generic model, but no evidence was available. Test it with the `text --suite correction` suite through `--llm-url` and a `--user-template` that matches its card. Accept it only if `unchanged_kept_rate` and `protected_token_rate` are at least the 4B baseline, `edit_vs_expected_mean` is not worse, and `wrong_script_count` is 0.

Prompt and sampling for the current model, from the code: `enable_thinking: false` (harmless for the 2507 Instruct model, which has no thinking mode), temperature 0.2. A temperature of 0 is the usual choice for minimal-edit tasks; run the correction suite with `--temperature 0` against 0.2 before changing it.

### C. Translation

| Model | Runtime | Evidence | Size | License |
|---|---|---|---|---|
| Qwen3-4B-Instruct-2507 Q4_K_M (current) | llama.cpp | General instruct model; local use | 2.50 GB | Apache-2.0 |
| **Tencent Hy-MT2-1.8B** (2026-05-21) | llama.cpp; official GGUF repos `tencent/Hy-MT2-1.8B-GGUF`, `-2bit-GGUF`, `-1.25bit-GGUF` | Vendor claims it beats commercial APIs (Microsoft, Doubao); 33 languages incl. ru/en; no independent RU↔EN numbers found (upstream) | 1.25-bit: 440 MB (vendor); Q4/Q8 sizes not read | GitHub repository: Apache-2.0. The Hugging Face weight license could not be read; earlier Hunyuan releases used a community license with territory restrictions, so check the card |

Hy-MT2 specifics from its README: no system prompt; one user message, "Translate the following text into {target_lang}. Note that you should only output the translated result without any additional explanation:"; a terminology template ("{src} translates to {tgt}"); recommended sampling temperature 0.7, top_p 0.6, top_k 20, repetition_penalty 1.05; context 8192. The 2-bit and 1.25-bit files need the STQ kernel from llama.cpp PR #22836; whether it is merged and in the pinned build is **unverified**, so use the standard Q4/Q8 GGUF first.

**One model or two?**

| | One general LLM (current) | Separate translation model |
|---|---|---|
| Processes | 1 | 2 (only one is active per session: the panel uses either correction or translation) |
| RAM while active | 4B: 2.5 GB weights + 1.1 GB KV (computed) | Translation session: 1.8B ≈ 1–1.2 GB Q4 weights + KV (expected); a mode switch keeps both until idle stop |
| Translation decode speed | baseline | ≈ 2× (CPU decoding is memory-bound; weight size 2.5 GB → ~1.1 GB) (expected) |
| Context document | Free text in the system prompt | Only glossary pairs fit the template; free text needs a background prompt the vendor documents only in Chinese |
| Complexity | none | second backend, port and compute group in the registry, new prompt builder |

Recommendation: measure first with `text --suite translation --llm-url ... --template hy-mt2`. Integrate as an optional backend only if chrF and the protected-token rate match the 4B and the latency gain holds on the laptop. Do not use the 4B for translation and Hy-MT2 for correction: Hy-MT2 is not a correction model.

### D. Speech synthesis and cloning

- **Kokoro 82M Q8_0** (Apache-2.0) stays the English voice. It has no Russian voice; Russian speech keeps using Qwen3-TTS with a profile, as now.
- **Qwen3-TTS-12Hz-0.6B-Base Q8_0** (Apache-2.0). audio.cpp's own CUDA report shows a peak of **6397 MiB VRAM** for Q8 in a long-lived session (upstream). Next to Dio on an 8 GB laptop GPU this may not fit or may spill into shared memory. The helper already stops after 120 s idle, which frees VRAM. Measure peak VRAM with `tts --voice <profile>` while Dio is loaded; if it does not fit, run cloning on CPU (`audiocpp_clone.enabled: false` and add the model to the main process) and accept slower synthesis.
- `qwen3_tts.voice_prompt_cache_slots: 4` caches conditioning per profile (default 1). Keep it.
- v0.9.3 adds an optimized Qwen3-TTS **streaming** mode: the first event carries one codec frame (80 ms of audio). Time to first audio could drop from a whole sentence to one frame (expected), but the gateway and `TtsPlayer` handle complete WAV files today. This is the main TTS latency lever; it needs streaming through the gateway and the player.

## 4. Comparison matrix

| Component | Current | Candidate | Quality evidence | Speed / resources | Runtime fit | Status |
|---|---|---|---|---|---|---|
| ASR | Qwen3-ASR 0.6B Q8_0 | Parakeet-TDT-0.6B-v3 Q8_0 | RU FLEURS 5.5–5.9 vs unknown (upstream) | Unknown on target CPU | Same audio.cpp process | Test locally (optional model) |
| ASR | Qwen3-ASR 0.6B Q8_0 | Nemotron 3.5 Streaming 0.6B | RU FLEURS 12.6 (upstream) | ~10× RT on a laptop CPU (upstream); streaming | audio.cpp streaming mode + live route | Not recommended now |
| Correction | Qwen3-4B Q4_K_M | loqira GEC 0.8B | Unknown | ~0.5 s/sentence expected (like Qwen3.5-0.8B) | llama.cpp | Test locally |
| Translation | Qwen3-4B Q4_K_M | Hy-MT2-1.8B Q4_K_M | Vendor claims only | ≈2× decode expected; about 1.4 GB smaller weights in translation sessions (expected) | llama.cpp (standard quants) | Test locally |
| KV cache | f16, ctx 8192 | q8_0, ctx 8192 | Generally reported as a small perplexity change in llama.cpp discussions; not verified for this task | −0.53 GiB (computed) | Config only | Test locally |
| TTS EN | Kokoro Q8_0 | — | — | — | — | Keep |
| TTS RU / clone | Qwen3-TTS 0.6B Base Q8_0, CUDA | Same model, audio.cpp v0.9.3 streaming | Same model | First audio after one codec frame (expected) | Needs gateway and player streaming | Later |
| Speech runtime | audio.cpp v0.9.2 | v0.9.3 | — | No documented x86 CPU gain for these models | Lock file update | A/B locally |

## 5. Recommended configurations

All three keep ASR, correction, translation and built-in TTS on the CPU and keep the cloning helper on demand.

**Minimum resources** (Dio running; RAM matters most):

```yaml
backends:
  audiocpp:   {threads: 4, idle_unload_ms: 90000}
  llama:
    external: true          # use Dio's server for text: no second LLM in RAM
    host: 127.0.0.1
    port: <Dio's port>
    model_alias: <Dio's model name>
  audiocpp_clone: {enabled: false}   # or keep it on demand if VRAM allows
```

Trade-off: text requests queue behind the agent's own work, the prompt goes to Dio's slots (the warm-up never touches an external server), and correction quality follows Dio's model. Without Dio running, text processing fails with a clear error.

**Balanced** (default hardware, Dio may run):

```yaml
backends:
  audiocpp: {threads: 4}
  llama:
    threads: 4
    cache_ram_mib: 512
    cache_type_k: q8_0      # after the correction suite shows no regression
    cache_type_v: q8_0
```

**Maximum quality, CPU-first:** the balanced block with `threads: 6–8` each when Dio is idle, Qwen3-4B for correction, and, if the translation suite confirms it, Hy-MT2-1.8B Q8_0 for translation. Keep Qwen3-ASR 0.6B unless Parakeet v3 wins on your recordings; Qwen3-ASR-1.7B exists in the same family but costs about three times the decoder compute.

Thread values are starting points (expected). Choose them from the thread sweep in section 9.

## 6. Integration considerations

- **API compatibility.** All changes are additive: `POST /v1/warmup` is new; `/v1/text/process` adds an optional `fallback` field and applies a cap only when `max_tokens` is absent. The panel ignores a `404` from an older service.
- **Lifecycle.** The warm-up uses normal leases, so idle stop and `idle_unload_ms` still apply. It skips resident models and external servers, and runs one warm-up per kind at a time.
- **External LLM.** `llama.external: true` still never starts or stops the adopted server, and is never warmed.
- **Compute switching.** Unchanged. Closing a process also closes its pooled HTTP client.
- **Parakeet as an optional model:** add a model entry with `family: parakeet_tdt`, `task: asr` to `backends.audiocpp.models` and select it with `model=` per request. It will ignore the context prompt.
- **A translation backend** would need a second `LlamaBackend`, a compute group, admin mapping and a Hy-MT2 prompt builder in `processing.py`. It was not implemented because its quality and license are unverified.

## 7. Implemented improvements

| Change | Files | Rationale | Verification |
|---|---|---|---|
| Pooled HTTP client per engine; health probe skipped for a live, healthy own child; one retry on a stale keep-alive connection | `backends/process.py`, `audiocpp.py`, `llama.py`, `whisper.py` | A5 | test: one TCP connection for three requests; crash detection still respawns. linux: p50 15.5 → 2.0 ms |
| Idle clock reset at spawn | `backends/process.py` | A6 | test |
| Stable prompt prefix | `processing.py` | A2 | test: context document inside the shared prefix for correct_ru, correct_en and translate |
| `--cache-ram 512`; optional `-ctk/-ctv` | `backends/llama.py`, `config.py`, `config.example.yaml` | A3, A13 | test: argument lists |
| Default generation cap; `fallback: "length"` | `api/text.py`, `docs/api.md` | A4 | test: cap applied, explicit limit honored, original text returned on runaway |
| `POST /v1/warmup` + panel call at session start | `api/warmup.py`, `backends/__init__.py`, `app.py`, `ServiceClient`, `MainWindow::startDictation` | A1 | test: ASR load and LLM prefix prefill, skip for dictation mode, external server and resident model; panel request body |
| Growing speculation interval | `DictationController.cpp`, `vad.py` | A7 | panel test fails on the old rule and passes on the new one; pause speculation unchanged |
| WebSocket partials, non-blocking ordered finals | `api/sessions.py`, `vad.py` | A8 | test fails on the old code (final before the second `speech_started`) and passes on the new code |
| Benchmark harness | `tools/bench/` | Sections 6 and 9 of the brief | test: metrics; text and ASR suites against the real app with fake engines; CLI smoke run |

Test results in this container: `python -m pytest tests -q` → 85 passed (64 before this work). Publication check: no findings. Panel: the changed `DictationController.cpp` and `ServiceClient.cpp` compiled with GCC 13 and Qt 6.4.2 against stubbed VAD/capture units; five controller and client tests passed, including two existing ones. `MainWindow.cpp` and the full `PanelTests` suite need Windows and were not built here.

## 8. Remaining work

Requires the laptop or a decision by the owner:

1. Run section 9 and decide KV `q8_0`, thread counts, and `silence_frames` (0.77 s is the largest fixed wait in the pipeline; 16 frames saves 0.26 s per sentence, but short pauses would split utterances).
2. A/B Parakeet v3, the loqira GEC model and Hy-MT2-1.8B with the harness.
3. Measure Qwen3-TTS peak VRAM next to Dio; decide GPU or CPU cloning.
4. Fix capture resampling (A11) and measure WER before and after.
5. Measure the ASR prompt cost (A10); if large, send a short terminology section instead of the whole document.
6. Evaluate audio.cpp v0.9.3, then Qwen3-TTS streaming playback.
7. Optional cleanups: separate TTS locks per process (A12), semaphore for streamed chat (A16), unused downloads (A15), the case-sensitive `#include <QJsonobject>` in `ServiceClient.cpp` (breaks non-Windows builds only).

## 9. Local benchmark instructions

Prepare a manifest with your own recordings (16 kHz mono WAV). Keep it under `runtime/bench/`, which Git ignores. Include short (1–3 s) and long (8–20 s) utterances, Russian, English and mixed technical speech, and tags:

```json
{"audio": "ru-mixed-01.wav", "text": "Открой блюпринт BP_PlayerCharacter и найди ивент BeginPlay.", "tags": ["ru", "mixed", "short"]}
```

Start the service and run each block twice: once with Dio idle or stopped, once while Dio runs an agent task. Every `llama-server`, `audiocpp_server` and `whisper-server` process is sampled and labelled with its port, so Dio's server appears as its own row (for example `llama-server.exe:port8000`).

```powershell
.\tools\start-service.ps1
```

```powershell
python tools/bench/run_bench.py asr --manifest runtime/bench/asr.jsonl --prefix-step 2 --label baseline
```

```powershell
python tools/bench/run_bench.py asr --manifest runtime/bench/asr.jsonl --prompt-file <context document path from Settings> --label asr-with-context
```

```powershell
python tools/bench/run_bench.py text --suite correction --repeat 3 --label baseline
```

```powershell
python tools/bench/run_bench.py text --suite translation --repeat 3 --label baseline
```

```powershell
python tools/bench/run_bench.py e2e --manifest runtime/bench/asr.jsonl --cold --repeat 1 --label cold
```

```powershell
python tools/bench/run_bench.py e2e --manifest runtime/bench/asr.jsonl --warmup --repeat 1 --label warmup
```

```powershell
python tools/bench/run_bench.py tts --voice <profile-id> --repeat 3 --label baseline
```

Candidate models through a hand-started llama-server (CPU build from the lock, port 8110):

```powershell
.\runtime\bin\llama-cpu\llama-server.exe -m <path to the Hy-MT2-1.8B Q4_K_M or Q8_0 GGUF> --port 8110 -c 4096 -t 4 --jinja --parallel 1 --cache-ram 512
```

```powershell
python tools/bench/run_bench.py text --suite translation --llm-url http://127.0.0.1:8110 --template hy-mt2 --sampling temperature=0.7,top_p=0.6,top_k=20,repeat_penalty=1.05 --label hy-mt2
```

Thread sweep (repeat for 2, 4, 6, 8; set `threads` for `audiocpp` and `llama` in `config.yaml`, restart the service, keep Dio running):

```powershell
python tools/bench/run_bench.py e2e --manifest runtime/bench/asr.jsonl --repeat 3 --label threads-4
```

KV cache quantization: set `cache_type_k: q8_0` and `cache_type_v: q8_0`, restart, and repeat the `text` suites and an `e2e` run.

Results are written to `runtime/logs/bench/`. Each file holds per-case outputs, latency p50/p95, and peak RSS, CPU and GPU memory per engine process.

### Acceptance criteria

| Metric | Accept a change when |
|---|---|
| ASR | `wer_mean` and every tag's WER within +0.5 points absolute of baseline; latency p95 not worse |
| Correction | `unchanged_kept_rate` ≥ baseline, `protected_token_rate` = 1.0, `edit_vs_expected_mean` ≤ baseline + 0.02, `wrong_script_count` = 0 |
| Translation | `protected_token_rate` = 1.0, `required_terms_missing` = 0, `chrf_mean` ≥ baseline − 1 |
| Latency | `speech_end_to_text_ms` p50 −20 % (model or setting change), or first-sentence p50 −20 % (`--warmup` against `--cold`) |
| Memory | Peak RSS of the changed engine reduced; `system_available_min_mib` not lower with Dio running |
| VRAM | `gpu_used_peak_mib` during dictation equals the idle value (no persistent VRAM); cloning peak fits next to Dio |

## Sources

Repository files are cited by path in the tables. External sources:

- audio.cpp README, server README, API and model docs (Qwen3-ASR, Qwen3-TTS, Kokoro, Parakeet-TDT, model licenses, Q8 performance report): <https://github.com/0xShug0/audio.cpp> (read at `main` and tag `v0.9.3`)
- audio.cpp v0.9.3 release notes: <https://github.com/0xShug0/audio.cpp/releases/tag/v0.9.3>
- llama.cpp server README at build b11871: <https://github.com/ggml-org/llama.cpp/blob/b11871/tools/server/README.md>
- Nemotron 3.5 ASR Streaming 0.6B model card (not readable from this environment): <https://huggingface.co/nvidia/nemotron-3.5-asr-streaming-0.6b>
- transcribe.cpp Nemotron 3.5 notes (WER and CPU figures): <https://github.com/handy-computer/transcribe.cpp/blob/main/docs/models/nemotron-3.5-asr-streaming-0.6b.md>
- NVIDIA NeMo-Speech.cpp: <https://github.com/NVIDIA/NeMo-Speech.cpp>
- Parakeet-TDT-0.6B-v3 model card: <https://huggingface.co/nvidia/parakeet-tdt-0.6b-v3>
- Canary-1B-v2 and Parakeet-TDT-0.6B-v3 paper: <https://arxiv.org/html/2509.14128v2>
- Qwen3-ASR technical report: <https://arxiv.org/pdf/2601.21337>
- Third-party Qwen3-ASR / Parakeet comparison (Apple Silicon, quantized): <https://soniqo.audio/benchmarks>
- Hy-MT2 repository and license: <https://github.com/Tencent-Hunyuan/Hy-MT2>
- Hy-MT2 1.25-bit GGUF card: <https://huggingface.co/AngelSlim/Hy-MT2-1.8B-1.25Bit-GGUF>
- loqira organization page: <https://huggingface.co/loqira>
