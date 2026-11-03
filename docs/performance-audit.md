# Performance and model audit

Date: 2026-11-03. Scope: the speech pipeline from microphone to inserted text, the text models, speech synthesis, and process lifecycles. Target: Windows 11, RTX 4060 Laptop (8 GB), 32 GB RAM, next to a local agent (Dio) that runs a large MoE model through llama.cpp.

## Summary

The pipeline design is sound: VAD-gated utterances, speculative recognition at pauses, ordered finals, sentence-level text processing, and separate engine processes. The audit found no reason to replace a default model now. Every candidate either lacks evidence on Russian, lacks a feature that VoicePanel uses (the ASR context prompt), or needs a local quality check that a cloud container cannot run.

The implemented changes remove fixed overhead and worst cases:

1. The panel asks the service to load the ASR model and prefill the text model's prompt when dictation starts (`POST /v1/warmup`), so the first sentence after an idle period should not pay the model cold start. Expected, not measured: see section 10.
2. The stable parts of the system prompt now come first, so a switch between Russian and English sentences should reuse the context document's KV cache instead of re-evaluating it. This follows from llama.cpp's documented `cache_prompt` behavior; it is not measured here (section 10), and its effect on correction quality needs the local A/B.
3. The llama-server prompt cache can no longer grow to its 8192 MiB default. It is bounded to 512 MiB.
4. A runaway generation cannot block the dictation queue for minutes. Requests without `max_tokens` get a cap of the input's UTF-8 byte length + 128 tokens. Reaching it is a visible failure (`502`, `reason: generation_limit`); the panel keeps the text unprocessed, shows it, and never inserts it as corrected or translated text.
5. Each gateway-to-engine call is about 13 ms faster at p50 (measured on Linux, see below).
6. Long utterances cost less speculative recognition (20 s utterance: 74 s → 35 s of re-recognized audio, simulated).
7. The WebSocket dictation route now produces partials, keeps listening while it recognizes, and keeps finals in order.

A review of the first round found a silent fallback (the panel inserted the unprocessed original as a successful translation), a generation cap that was too small for long Russian text, and misleading benchmark names. Section 10 lists those corrections and what is and is not verified.

The 20 % end-to-end target is expected (not measured) for the first sentence of a session after idle; the panel timing log in section 9 measures it. It is not expected for steady-state sentences from these changes alone. The largest remaining levers are model-level and need the laptop: a dedicated translation model (Hy-MT2-1.8B), KV cache quantization, the VAD end-of-speech wait, and thread allocation next to Dio. The benchmark harness in `tools/bench/` measures each of them.

### Evidence labels

| Label | Meaning |
|---|---|
| **code** | Verified by reading the source in this repository |
| **test** | Executed in this audit (Linux container, model-free) |
| **linux** | Measured in the cloud container: 4-core Intel Xeon 2.1 GHz, 15 GB RAM, no GPU, no models. Not a Windows or RTX 4060 result |
| **upstream** | Published by a model vendor, runtime project or third party; not reproduced here |
| **computed** | Derived arithmetic from published architecture or configuration values |
| **simulated** | Computed by a model of the code's rules, not by running the system |
| **expected** | A prediction; needs the local benchmark |
| **panel** | Measured by the panel's timing log on the target laptop (none yet) |

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
| A2 | The system prompt put the per-request task line before the context document. In auto mode the task changes with each sentence's language, so llama.cpp could reuse only the first rules block and re-evaluated the context document on every RU/EN switch. | `processing.build_messages` | code, test | Medium; grows with the context document | **Changed**: stable parts first. Reuse expected from llama.cpp's documented `cache_prompt`; not measured (section 10) |
| A3 | llama-server b11871 keeps a host-RAM prompt cache with an 8192 MiB default and saves idle slots into it. Different prompts (other modes, `/v1/chat/completions` clients) can grow the process by gigabytes. | llama.cpp server README at b11871 | upstream, code | Medium (memory pressure next to Dio) | **Fixed**: `cache_ram_mib: 512` |
| A4 | No generation limit when the client sends no `max_tokens`. A repetition loop (seen before with `previous`) runs until the 8192-token context fills, for minutes of CPU time, while later sentences wait. | `api/text.py` | code | Medium (rare, severe) | **Fixed**: derived cap; reaching it is a `502` failure (corrected in A17, A18) |
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
| A17 | First-round regression: at the derived cap the service answered `200` with the input as `result`. The panel treated it as processed text, so Quick Dictation could insert Russian input as an English translation. | `api/text.py`, `ServiceClient`, `TextProcessor` | code, test (fails on the first-round code) | High (silent wrong output) | **Fixed**: `502` with `reason`, `original`, no `result`; panel failure path |
| A18 | First-round cap `chars / 2 + 128` was below the needed output for long Russian and digit-heavy text (0.80–0.99× of the need, measured with the Qwen vocabulary). | `api/text.py` | measured token counts (section 10) | Medium (legitimate long text would fail) | **Fixed**: UTF-8 bytes + 128 |
| A19 | Warm-up checked model residency before taking the ASR lock; after a real request loaded the model, warm-up still sent its silent request and delayed the next real one. | `api/warmup.py` | test (fails on the first-round code) | Low | **Fixed**: re-check inside the lock |
| A20 | The first-round probe skip meant a hung (alive, not answering) engine was only detected at the request timeout (300 s) instead of by the next probe. | `backends/process.py` | code, test (fails on the first-round code) | Low–medium | **Fixed**: a transport error clears the cached health state |
| A21 | The benchmark's `e2e` suite and `speech_end_to_text_ms` metric named an ASR request plus a text request as end-to-end latency. Text-processing failures crashed the suites. | `tools/bench/run_bench.py` | code | Medium (misleading results) | **Fixed**: `chain` suite, `asr_plus_text_request_ms`, a labelled estimate, failure counts; panel timing log for real end-to-end |

Checked and found adequate: VAD runs on one ONNX thread per stream; capture runs on its own thread and is never blocked by HTTP; the panel's `QNetworkAccessManager` reuses connections; finals and insertions keep order; failed text processing keeps the original text; edit protection (`userEditedResult_`, revision checks) is intact. Speech synthesis already pipelines two sentences ahead of playback.

### Latency model of one dictated sentence

**code + simulated; not measured.** After the speaker stops: the VAD decides after 0.77 s. The final ASR started at 0.19 s, so recognition adds `max(0, ASR time − 0.58 s)`. Correction follows (recorded locally as about 1.3 s per sentence with Qwen3-4B; historical, not verified here). Insertion is a clipboard paste. For a typical 3–5 s utterance at about 5× real time, ASR takes 0.6–1.0 s, so the speech-end-to-text time is roughly 0.8–1.2 s + 1.3 s ≈ 2.1–2.5 s. Text processing is the largest part. Translation mode is similar.

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

- **API compatibility.** `POST /v1/warmup` is new; the panel ignores a `404` from an older service. `/v1/text/process` is unchanged when the client sets `max_tokens` and when the model finishes normally. Without `max_tokens`, a model that reaches the derived cap now gets `502` with `reason: generation_limit` and the submitted `original`, where `main` returned the looping output with `200`. Older panels handle the `502` with their existing failure path, which keeps the text unprocessed and does not insert it.
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
| Default generation cap (UTF-8 bytes + 128); `502 generation_limit` with `original` | `api/text.py`, `docs/api.md` | A4, A17, A18 | test: cap value, explicit limit honored, success paths for correction and translation, limit and backend failures without `result` |
| Visible processing failure in the panel | `ServiceClient.cpp`, `TextProcessor.{h,cpp}`, `MainWindow.cpp` | A17 | Linux/Qt 6.4: processor keeps the original out of processed batches and the final tail; mode follows the sentence language. `MainWindow` test written, Windows only |
| Warm-up re-check inside the ASR lock; health reset on transport errors; warm-up cancellation at shutdown | `api/warmup.py`, `backends/process.py`, `backends/__init__.py` | A19, A20 | test: live-server concurrency tests (two fail on the first-round code) |
| Request-level `chain` suite, `prefix` suite, legacy prompt template, failure counting; panel timing log and analyzer | `tools/bench/`, `TimingLog.{h,cpp}`, `DictationController.cpp`, `TextProcessor.cpp`, `MainWindow.cpp`, `CMakeLists.txt` | A21 | test: estimate formula, output schema, failure handling, prefix suite against a cache-simulating fake server, log analyzer; timing log compiled and written on Linux/Qt 6.4 |
| `POST /v1/warmup` + panel call at session start | `api/warmup.py`, `backends/__init__.py`, `app.py`, `ServiceClient`, `MainWindow::startDictation` | A1 | test: ASR load and LLM prefix prefill, skip for dictation mode, external server and resident model; panel request body |
| Growing speculation interval | `DictationController.cpp`, `vad.py` | A7 | panel test fails on the old rule and passes on the new one; pause speculation unchanged |
| WebSocket partials, non-blocking ordered finals | `api/sessions.py`, `vad.py` | A8 | test fails on the old code (final before the second `speech_started`) and passes on the new code |
| Benchmark harness | `tools/bench/` | Sections 6 and 9 of the brief | test: metrics; text and ASR suites against the real app with fake engines; CLI smoke run |

Test results in this container after the post-review corrections: `python -m pytest tests -q` → 128 passed (64 on `main`). Publication check: no findings. Panel: `DictationController.cpp`, `ServiceClient.cpp`, `TextProcessor.cpp` and `TimingLog.cpp` compiled with GCC 13 and Qt 6.4.2 against stubbed VAD and capture units; 13 controller, client and processor tests extracted from `PanelTests.cpp` passed, with and without `VOICEPANEL_TIMING_LOG`. `MainWindow.cpp`, the full `PanelTests` suite (including the new `failedTranslationIsVisibleAndNotProcessed`), MSVC 2022 and Qt 6.8.3 need Windows and were not built here.

## 8. Remaining work

Requires the laptop or a decision by the owner:

1. Run section 9 and decide KV `q8_0`, thread counts, and `silence_frames` (0.77 s is the largest fixed wait in the pipeline; 16 frames saves 0.26 s per sentence, but short pauses would split utterances).
2. A/B Parakeet v3, the loqira GEC model and Hy-MT2-1.8B with the harness.
3. Measure Qwen3-TTS peak VRAM next to Dio; decide GPU or CPU cloning.
4. Fix capture resampling (A11) and measure WER before and after.
5. Measure the ASR prompt cost (A10); if large, send a short terminology section instead of the whole document.
6. Evaluate audio.cpp v0.9.3, then Qwen3-TTS streaming playback.
7. Build and run the panel suite on Windows (commands in the merge checklist below) and run the prompt-order A/B and the `prefix` suite before merging.
8. Decide whether the translation record should keep untranslated tails out of the `translation` field (section 10, residual risk).
9. Optional cleanups: separate TTS locks per process (A12), semaphore for streamed chat (A16), unused downloads (A15), the case-sensitive `#include <QJsonobject>` in `ServiceClient.cpp` (breaks non-Windows builds only).

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
python tools/bench/run_bench.py chain --manifest runtime/bench/asr.jsonl --cold --repeat 1 --label cold
```

```powershell
python tools/bench/run_bench.py chain --manifest runtime/bench/asr.jsonl --warmup --repeat 1 --label warmup
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
python tools/bench/run_bench.py chain --manifest runtime/bench/asr.jsonl --repeat 3 --label threads-4
```

KV cache quantization: set `cache_type_k: q8_0` and `cache_type_v: q8_0`, restart, and repeat the `text` suites and a `chain` run.

The `chain` suite is request-level: one ASR request for the whole utterance plus one text request, timed at the client. It excludes the VAD end-of-speech wait, the overlap with speculative recognition, capture buffering, GUI scheduling and insertion. Its `estimate` block applies the panel's VAD rule to those request times (`max(silence_frames × 32 ms, 192 ms + ASR) + text`); it is a model estimate, not a measurement. Use the panel timing log below for user-facing latency.

Prompt order A/B on the current Qwen3-4B, through the service's own llama-server (start the service and call `/v1/warmup` once so the server runs):

```powershell
python tools/bench/run_bench.py text --suite correction --repeat 3 --llm-url http://127.0.0.1:8103 --sampling temperature=0.2 --template voicepanel-legacy --label order-legacy
```

```powershell
python tools/bench/run_bench.py text --suite correction --repeat 3 --llm-url http://127.0.0.1:8103 --sampling temperature=0.2 --template voicepanel --label order-current
```

Repeat both with `--suite translation`. `voicepanel-legacy` rebuilds the prompt exactly as `main` did (verified identical for all four modes). The two runs use the same model, sampling and cases; only the order of the system-prompt parts differs.

Prefix reuse and warm-up, from llama-server's own counters (`timings.prompt_n` evaluated, `timings.cache_n` reused):

```powershell
python tools/bench/run_bench.py prefix --context-file <context document path from Settings> --repeat 2 --label prefix
```

### Panel end-to-end latency (Windows)

This is the only measurement of speech end to inserted text. Close the panel, then:

```powershell
$env:VOICEPANEL_TIMING_LOG = "$PWD\runtime\logs\bench\panel-timing.jsonl"
```

```powershell
.\tools\start-panel.ps1
```

Open Notepad, focus it, start Quick Dictation (Ctrl+Alt+D), and say 20 complete sentences, one per utterance, each followed by a pause of at least two seconds. Mix Russian, English and technical sentences. Stop, close the panel, and remove the variable (`Remove-Item Env:VOICEPANEL_TIMING_LOG`). Then:

```powershell
python tools/bench/panel_timing.py runtime/logs/bench/panel-timing.jsonl --json runtime/logs/bench/panel-timing-summary.json
```

The analyzer reports, per utterance, `vad_wait_ms`, `speech_end_to_final_ms`, `final_to_text_ms`, `text_to_insert_ms`, `speech_end_to_text_ms` and `speech_end_to_insert_ms` (p50/p95). "Speech end" is when the GUI thread processed the last VAD frame above the threshold; the acoustic end is earlier by the 64 ms capture block, the device period and VAD lag, so all figures are slightly low. Utterances whose finals were batched into one text request are reported as `ambiguous` and left out. Repeat with Dio running. Compare a change by running the protocol before and after it.

Results are written to `runtime/logs/bench/`. Each file holds per-case outputs, latency p50/p95, and peak RSS, CPU and GPU memory per engine process.

### Acceptance criteria

| Metric | Accept a change when |
|---|---|
| ASR | `wer_mean` and every tag's WER within +0.5 points absolute of baseline; latency p95 not worse |
| Correction | `unchanged_kept_rate` ≥ baseline, `protected_token_rate` = 1.0, `edit_vs_expected_mean` ≤ baseline + 0.02, `wrong_script_count` = 0 |
| Translation | `protected_token_rate` = 1.0, `required_terms_missing` = 0, `chrf_mean` ≥ baseline − 1 |
| Latency (user-facing) | Panel timing log: `speech_end_to_insert_ms` p50 −20 % for a model or setting change |
| Latency (first sentence) | `chain`: `asr_plus_text_request_ms` p50 with `--warmup` at least 20 % below `--cold` |
| Prefix reuse | `prefix`: `current.prompt_n_after_language_switch` p50 below `legacy`; `first_request_after_reset.with_warmup.cache_n` above `without_warmup` |
| Prompt order | `order-current` meets the Correction and Translation rows against `order-legacy` |
| Failures | `failures` / `text_failures` = 0 in every text and chain run |
| Memory | Peak RSS of the changed engine reduced; `system_available_min_mib` not lower with Dio running |
| VRAM | `gpu_used_peak_mib` during dictation equals the idle value (no persistent VRAM); cloning peak fits next to Dio |

## 10. Post-review corrections and verification status

### Generation cap

Method (measured in the container; tokenizer only, no model): the Qwen byte-level BPE vocabulary (`qwen.tiktoken`, 151,643 tokens, from the `dashscope` PyPI package; the Qwen2 and Qwen3 reports state that they reuse this base vocabulary, not re-verified here) with the Qwen2 pre-tokenizer pattern. Inputs: eight paired Russian/English technical sentences and paragraphs (48–341 characters) written for this check, plus one digit-heavy pair with ports, versions and an IP address.

| Measure | Result |
|---|---|
| Tokens per character, prose | Russian 0.32 (0.28–0.36), English 0.20 (0.18–0.25) |
| Tokens per character, digit-heavy | Russian 0.62, English 0.51 (Qwen spends one token per digit) |
| Old cap `chars / 2 + 128`, worst short-text margin | 2.49× (Russian correction) |
| Old cap, long-text margin without the +128 (worst direction, digit-heavy) | 0.80× RU correction, 0.88× EN→RU, 0.90× RU→EN, 0.99× EN correction: a legitimate long output would hit the cap |
| New cap `UTF-8 bytes + 128`, long-text margin (worst) | 1.76× (EN→RU, digit-heavy); 2.17× RU correction; 2.43× RU→EN |
| Runaway budget for a 120-character sentence | Russian 188 → 368 tokens; English 188 → 248 tokens; both far below the 8192-token context |

A correction of the same length can never reach the new cap, because byte-level BPE emits at most one token per byte. The old cap was replaced because it was below the need, not to make it larger for its own sake.

### Processing failure path

**code, test.** At the derived cap the service answers `502` with `reason: generation_limit`, `max_tokens` and `original`, and no `result`. In the panel, `ServiceClient` turns the reason into a message; `TextProcessor` keeps the batch in `pendingOriginal()`, emits `failed` and the new `processingFailed(original, error)`, and stops dispatching until the next transcript or a retry, as for any backend failure. It never emits the text as a processed batch or as the final tail. Automatic insertion sends only `processedSoFar_` plus the final tail, so the unprocessed text cannot reach the target. `MainWindow` shows "Correction stopped" or "Translation stopped", says the text is kept and not inserted, points to Retry correction, Review and Copy, and expands the panel. The original pane always has the text; the result pane shows it at the end, unprocessed. History, insertion progress and silence rollover are unchanged: rollover preserves the transcript (with its original) and resets the processor. The manual Translate button already kept the text on failure.

Residual risk (pre-existing behavior, now also reached by this failure): in translation mode, the result pane, the History entry and the auto-saved record's `translation` field contain the untranslated tail after the translated part. The label states this. Changing what `translation_` holds would also change the panel's user-edit detection, which cannot be tested here.

### Warm-up review

| Check | Result | Evidence |
|---|---|---|
| Warm-up and the first ASR request | The real request queues behind the warm-up's load on `_asr_lock` (FIFO) and returns its own transcript, never the warm-up's | test on a live server |
| Real request loads the model first | Warm-up re-checks residency inside the lock and sends nothing | test (fails on the first-round code) |
| Warm-up and the first text request | Both complete with their own results; llama-server (`--parallel 1`) queues the second | test on a live server (gateway level) |
| Duplicate calls | One warm-up per kind at a time; later calls report `scheduled: []` | test |
| Resident model | No ASR request | test |
| Failed or missing engine | The error is logged at info level, the state is cleared, a later warm-up runs again, the service stays healthy | test (connection error, missing binary) |
| Dictation without correction | ASR only | test |
| External llama.cpp server (Dio) | Never warmed: no request reaches it | test |
| Whisper as ASR default | No audio.cpp request | test |
| Shutdown during warm-up | Cancelled before the engines stop | test (passes on both versions: the test client's loop also cancels leftover tasks) |
| Session cancelled during warm-up | Warm-up continues in the background; the loaded model and a started llama-server then follow the normal idle rules (`idle_unload_ms`, `idle_stop_s`). No restart is triggered | code |
| Wrong transcripts | Warm-up output is discarded; it shares no state with real requests except the engine | code, test |

**Prefix caching is expected, not verified.** The pinned llama-server README documents `cache_prompt` (default on): "the prompt is compared to the previous completion and only the unseen suffix is evaluated", and reports the counts as `timings.cache_n` and `prompt_n`. With `--parallel 1` all requests use one slot. After the reorder, every mode shares the system prompt up to the task line (pinned by a test), so the warm-up request and the first sentence should share that prefix. Three things can change the outcome and were not observed: another client of the same llama-server between the two requests replaces the slot's prompt; `--cache-idle-slots` and the 512 MiB `--cache-ram` cache save and restore slot states in ways only the server's counters show; and tokens at the boundary of the shared text can differ. The harness tested the mechanism only against a fake server that simulates `cache_prompt` on characters. Run the `prefix` suite on the laptop to confirm it.

### Prompt order

Tests pin the layout for all four modes and all six profiles: rules first, then profile, terminology, context document, and the task line last before optional spell-checker hints; each mode names its languages; correction forbids additions and allows an unchanged result; the user turn is the text only; everything before the task line is identical across modes. These tests show structure, not quality. Moving the task after a long user-written context document could change how the model weighs it; the instruction is now the last part of the system prompt, closest to the text, which usually helps adherence, but that is a prediction. No claim of unchanged quality is made: run the A/B in section 9 (`voicepanel-legacy` against `voicepanel`) before merging.

### Concurrency and resources

| Area | Review result |
|---|---|
| Pooled HTTP client | One client per engine and event loop, closed when the process stops. A request on a stale keep-alive connection is retried once; a second failure is an error (test). Keep-alive expiry 4 s, below cpp-httplib's 5 s |
| Health and adoption | Probe skipped only for a live child that passed a health check; a crash respawns (test); a transport error forces the next probe (fixed, test). Adopted and external servers are probed as before |
| Leases and idle timers | Lease counts are balanced on success and failure (test); spawning resets the idle clock (test); the fast path has no await between start and the lease increment, so the idle watcher cannot stop the process in between |
| WebSocket finals | Ordered; a failed final sends `error` and later finals still arrive in order; `cancel` drops pending finals and releases the ASR lock (tests) |
| Background tasks | WebSocket tasks cancelled on finish, cancel and disconnect; warm-ups cancelled at shutdown |
| Speculation interval | Pause speculation unchanged; periodic interval grows (panel test fails on the old rule) |
| Generation limits | See above |

### Verified here and pending on Windows

| Item | Status |
|---|---|
| Service (Python 3.13 here; project targets 3.11–3.12) | All tests run |
| `DictationController.cpp`, `ServiceClient.cpp`, `TextProcessor.cpp`, `TimingLog.cpp` | Compiled with GCC 13 and Qt 6.4.2 against stubbed VAD and capture units; 13 extracted panel tests passed, with and without the timing log |
| `MainWindow.cpp`, the full `PanelTests` suite, MSVC 2022, Qt 6.8.3 | Not built or run: Windows only |
| GUI behavior, insertion, inference speed, prefix reuse, quality of the prompt order | Not verified: needs the laptop and the steps in section 9 |

### Merge checklist (Windows)

Run from a Visual Studio 2022 Developer PowerShell in the repository root, with Qt 6.8.3 for MSVC 2022 installed as in [Getting started](getting-started.md).

```powershell
git fetch origin
```

```powershell
git switch claude/brave-cori-1idyww
```

```powershell
python -m pip install --only-binary=:all: --require-hashes -r service/requirements.lock
```

```powershell
python -m pip install pytest==9.1.1
```

```powershell
python -m pytest tests -q
```

```powershell
python tools/verify_publication.py
```

```powershell
.\tools\build_panel.ps1
```

```powershell
ctest --test-dir panel/build --output-on-failure
```

The new panel tests by name (the build script copies the VAD model next to the executable):

```powershell
Push-Location panel\build; .\bin\VoicePanelTests.exe -platform offscreen failedTranslationIsVisibleAndNotProcessed generationLimitKeepsTheOriginalOutOfProcessedText processingModeFollowsTheSentenceLanguage periodicSpeculationWaitsForTheSegmentToGrow pauseSpeculationIsNotDelayedByTheGrowthRule warmupCarriesModeAndContext; Pop-Location
```

Manual failure-path check with the real panel: stop the service, copy `config.yaml` to `config.failtest.yaml`, set `backends.llama.external: true` and `backends.llama.port: 8199` (nothing listens there), start it with `.\tools\start-service.ps1 -Config config.failtest.yaml`, enable Auto-translate, and dictate a Russian sentence into Notepad with Ctrl+Alt+D. Expected: nothing is inserted, the panel expands with "Translation stopped", the original pane holds the sentence, and Copy copies it. Stop the service, delete `config.failtest.yaml` (Git does not ignore it), start the service with `config.yaml`, choose Retry correction from the result pane's context menu, and insert. Then run the manual acceptance list in [Testing](testing.md), the prompt-order A/B and the `prefix` suite (section 9), and one panel timing protocol run.

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
