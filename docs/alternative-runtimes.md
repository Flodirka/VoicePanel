# Alternative runtimes and models - evaluation notes (2026-11, follow-up included)
#
# All artifacts are local (models under runtime/models/, samples and logs under

## FINAL STATE (2026-11-06) — read this first

The open questions in the log below are resolved as follows:

* **OmniVoice in the service**: done through the omnivoice.cpp engine, not
  audio.cpp. A new `OmnivoiceBackend` (service/voiceservice/backends/omnivoice.py)
  supervises `omnivoice-tts --stream-by-line -o -` (one utterance per stdin
  line, RIFF-split + two-phase stall detection), pre-encodes profile
  references to .rvq with `omnivoice-codec`, and follows the `clone` compute
  group's CPU<->CUDA switch. The CUDA build (GGML_CUDA=ON, sm_89) lives in
  runtime/bin/omnivoice-cuda and needs cublas64_12.dll (taken from the
  nvidia-cublas-cu12 wheel). Measured RTF on the busy RTX 4060: auto 1.9
  (steps 8) / 4.5 (steps 16), clone 6.9 (steps 16); CPU: 13.1 / 24.2.
* **Cloning default**: `tts.clone_model: omnivoice-q8`; qwen3-tts-clone is the
  documented alternative (`audiocpp_clone`, disabled by default).
* **Built-in Russian voices**: Piper Ruslan (male) + Irina (female, converted
  from rhasspy/piper-voices ONNX with audio.cpp's converter). Irina measured
  RTF 0.47 on the busy CPU; `tts.default_voices.ru` accepts a built-in model
  id, so the panel/user can pick the voice per language.
* **CUDA toolkit**: nvcc 12.9 + cudart + cccl under runtime/cuda-toolkit/root,
  plus cuBLAS from the pip wheel whose .lib import libraries were generated
  with dumpbin/lib (the wheel ships none). The NeMo-Speech.cpp CUDA build
  still fails to configure (unrelated); it stays external by decision.
* audio.cpp still cannot load the split OmniVoice GGUFs (tensor names longer
  than stock ggml's 64-char limit) — and no longer needs to.

The sections below are the historical evaluation log that led here.

## 0. CUDA on this machine — clarified (follow-up)

The GPU was never missing: RTX 4060 Laptop, driver 621.01, `nvidia-smi` works, and the
prebuilt CUDA engine builds (`runtime/bin/llama-cuda`, `runtime/bin/audiocpp-cuda`) run
on the driver alone — that is exactly what the settings panel's CPU/GPU switch uses
(`compute.py` swaps binaries / `gpu_layers`). Verified live after restoring `llama-cuda`
(GEC 0.8B one-shot on GPU, `-ngl 99`, VRAM tracked via nvidia-smi; note the 8 GB card is
mostly held by the concurrent agent process Dio, ~6.7 GB).
What was missing is the CUDA **toolkit** (`nvcc`) — needed only to compile source builds
(NeMo-Speech.cpp, omnivoice.cpp), not to run inference. It was absent from PATH and disk;
the pip `nvidia-cuda-nvcc-cu12` wheel ships only `ptxas`, not `nvcc`. A real nvcc 12.9.86
was installed from NVIDIA's public redist (`cuda_nvcc` windows archive, 127 MB) into
`runtime/cuda-toolkit/root` (with cudart + cccl); `nvcc --version` works there.
My earlier `cleanup_runtime.ps1` run had wrongly deleted `runtime/bin/llama-cuda` —
restored via `tools/download_runtime.py` (SHA-256-verified) and the keep-list now
includes all GPU-capable builds (`llama-cuda`, `audiocpp-cuda`, `audiocpp-vulkan`).

## NeMo-Speech.cpp CUDA build status

`scripts/windows/build.ps1 -Backend cuda -Profile asr` with `CUDA_PATH` pointing at the
redist toolkit: vcpkg bootstrapped, MSVC env imported, compile started (log:
`runtime/logs/nemo-cuda-build2.log`). Earlier CPU-only attempt failed with exactly
"nvcc not found; install a supported NVIDIA CUDA Toolkit" — now resolved. Build runs in
background; result recorded in the log. Regardless of outcome, the app keeps audio.cpp
(see decision in section 1).

## NeMo + Silero VAD streaming (follow-up)

`silero-vad-F32.gguf` (1.2 MB, Serveurperso/Silero-VAD-GGUF) added to
`runtime/models/silero-vad-v6.gguf`; `nemo-speech serve --vad-model ...` accepts it.
Realtime WS stream of `mixed-16k.wav` with VAD+endpointing: deltas every ~50-100 ms,
`completed` at +3.75 s on the 6.96 s file (vs +2.30 s without VAD on the earlier pass —
endpointing timing varies with load). Silero + streaming + diarization all work in the
installed 0.3.0 build.


## 1. NeMo-Speech.cpp vs audio.cpp — real streaming comparison (follow-up)

Setup: `nemo-speech serve --asr-model nemotron-3.5 --diar-model nemotron-3-diarization --port 8200 --device cpu` (realtime WS `/v1/audio/transcriptions/realtime`: `session.update` then binary PCM16 frames; deltas `conversation.item.input_audio_transcription.delta`, `...completed`) vs a test audio.cpp server (port 8120) with `nemotron` in `mode: streaming` and chunked POST `/v1/audio/transcriptions/live` (SSE). Same input (`mixed-16k.wav` 6.96 s, 250 ms chunks; long `long-16k.wav` 26.7 s).

- Both stream incrementally without closing the stream; endpointing works on both. NeMo emits finer deltas (~50-100 ms) than audio.cpp (~170 ms).
- First-token latency, short file: NeMo +0.35 s vs audio.cpp ttft_ms ~1003. NeMo's cache-aware RNN-T streaming is quicker to first token.
- Long session (26.7 s): both keep state; audio.cpp finished at +18.5 s (79 deltas, full correct text, RTF ~0.69); NeMo correct incrementally (wall time inflated by the shared loaded CPU).
- RAM: nemo-speech ~1017 MB resident; audio.cpp evicts idle models (`idle_unload_ms`).
- Compatibility: NeMo-Speech.cpp rejects audio.cpp GGUFs (`architecture='audiocpp'`); it pulls its own pinned copies from `nvidia/*` on HF (OpenMDW-1.1). Diarization via `--diar-model nemotron-3-diarization` / `POST /v1/audio/diarizations` (streaming default, offline to ~6.6 min). Silero VAD via `--vad-model` (GGUF).
- **CUDA build blocked**: no CUDA toolkit here (no `nvcc`/`CUDA_PATH`); installed 0.3.0 binary is CPU-only.
- Caveat: this box runs a large agent LLM concurrently; wall times fluctuate. TTFT was stable: NeMo ~0.35 s, audio.cpp ~1.0 s.
- **Decision: keep audio.cpp.** NeMo's TTFT edge doesn't pay off given a separate process, duplicate model copies, and its own API; VoicePanel's VAD+speculation already hides most latency (final starts ~0.58 s before VAD end). NeMo is not wired into the service.

# runtime/logs/); nothing was added to git.
#
# See sections below. (Full content written by parts.)

# Alternative runtimes and models - evaluation notes (2026-11, follow-up included)
#
# All artifacts are local (models under runtime/models/, samples and logs under

## FINAL STATE (2026-11-06) — read this first

The blockers below are resolved as follows:

* **OmniVoice in the service**: done through the omnivoice.cpp engine, not
  audio.cpp. A new `OmnivoiceBackend` (service/voiceservice/backends/omnivoice.py)
  supervises `omnivoice-tts --stream-by-line -o -` (one utterance per stdin
  line, RIFF-split + stall detection), pre-encodes profile references to .rvq
  with `omnivoice-codec`, and is switched CPU<->CUDA by the `clone` compute
  group. The CUDA build (GGML_CUDA=ON, sm_89) lives in runtime/bin/omnivoice-cuda
  (needs cublas64_12.dll — generated from the nvidia-cublas-cu12 wheel).
  Measured RTF on the busy RTX 4060: auto 1.9 (steps 8) / 4.5 (16), clone 6.9.
  audio.cpp cannot load the split GGUFs (tensor names > 64 chars) — that gap
  stands, and the packed-GGUF route is no longer needed.
* **Cloning default**: `tts.clone_model: omnivoice-q8`; qwen3-tts-clone is the
  documented alternative (audiocpp_clone, disabled by default).
* **Built-in Russian voices**: piper Ruslan (male) + Irina (female, converted
  from rhasspy/piper-voices ONNX with audio.cpp's converter). Irina measured
  RTF 0.47 on the busy CPU. `tts.default_voices.ru` accepts a built-in model id.
* **CUDA toolkit**: nvcc 12.9 + cudart + cccl + cuBLAS (from the pip wheel)
  live under runtime/cuda-toolkit/root; import libs for cuBLAS were generated
  from the wheel DLLs (dumpbin/lib) because the wheel ships none.
* NeMo-Speech.cpp stays external (decision unchanged); audio.cpp serves ASR.

The sections below are the historical evaluation log that led here.



## 2. OmniVoice Q4_K_M vs Q8_0 (follow-up: clones, cache, GPU)

- CUDA **blocked** (no CUDA toolkit); all numbers CPU (8 threads, shared box, `--steps 16`).
- Files: `runtime/models/omnivoice/` — split GGUFs (base Q4 407 / Q8 656 MB + tokenizer Q4 252 / Q8 289 MB) + merged `omnivoice-q8_0-merged.gguf` (1.47 GB). Both quats kept.
- omnivoice.cpp built from source (CPU). Samples in `runtime/logs/omnivoice/`: `ov-{q4,q8}-{en,ru}.wav`, `ov-q8-clone-ru.wav`, `ov-q4-clone-ru.wav`, `ov-q8-clone-cached{,2}.wav`.
- Auto voice CPU RTF: Q8 EN 10.6 / RU 13.1; Q4 EN 7.9 / RU 9.6. Clone RU: Q8 53.7 s (RTF 17.0), Q4 28.2 s (RTF 8.9) — Q4 ~1.9x faster at same settings.
- Voice-ref caching works: `omnivoice-codec` pre-encodes the ref to a tiny `.rvq` (6.4 s ref -> 1727 bytes, ~2 s); `--ref-rvq` skips the per-synthesis encode. Cached-run wall times were load-confounded (76-92 s); saved work is the encode step.
- RAM: Q8 ~620 MB LM + 289 MB codec; Q4 ~407 + 252 MB.
- audio.cpp gap: `omnivoice` family needs ONE packed GGUF (`weights.*`/`audio_tokenizer_weights.*`) in a package dir; the split tokenizer GGUF exceeds stock ggml's 64-char tensor-name limit and the merged file uses plain names (rejected). Repack must come from omnivoice.cpp's ggml fork. **omnivoice.cpp is the working engine today.**
- vs qwen3-tts-clone: 1.9 GB on the GPU helper (~6.4 GB VRAM peak per audio.cpp CUDA report); OmniVoice Q8 ~945 MB total, clones from raw WAV + transcript. qwen3 CLI cloning needs a voice-library profile (exit 3 without one).
- License: CC-BY-NC 4.0 — personal test OK, **not shippable** in the public app.
- **Chosen variant: Q8_0** (user listened: noticeably better than Q4). Q4 files and the experimental merged GGUF were removed after the choice; Q8 pair kept.

## 3. PLaMo 3 Translate 0.8B — context/glossary fixed (follow-up)

- Root cause of the system echo: PLaMo's GGUF chat template echoes a leading system turn; documented options are `context`/`glossary`/`source_lang`/`target_lang` as chat-template kwargs.
- Fix (config-gated, generic): backends accept `template_style: plamo`; then `/v1/text/process` sends no system message and passes `context` (context document), `glossary` (dictionary terms), and `target_lang` (now auto for all translate modes; other templates ignore it). Backends also accept config-level `chat_template_kwargs`.
- Verified in the running bundle service (`llama_translate` -> PLaMo Q4_K_M + `template_style: plamo`): no echo; RU->EN keeps `body_entered`, `Area2D`, `main.cpp` (identity glossary pairs lowercased `LoadConfig` — use real source->target pairs for case-sensitive terms); EN->RU keeps `MonoBehaviour`, `Awake`, `queue_free`. Hy-MT2 on the same cases: identifiers preserved equally.
- Latency (loaded box): PLaMo 32.8 s cold / 4.3 s warm; Hy-MT2 32-122 s same load; idle bench p50 for Hy-MT2 was 516 ms. **No reliable speed edge under load** — re-run idle before deciding.
- Selection: config-only swap of `llama_translate.model_path` + `template_style: plamo`; verified through `/v1/text/process` in the bundle. Hy-MT2 unchanged. License: PLaMo Community License.

## 4. Final validated settings

- ASR/diar: audio.cpp CPU, `nemotron` + `nemotron-diar`, Silero VAD client-side, speculation on. Keep.
- Correction: `llama` = GEC 0.8B, `enable_thinking: false`; deterministic pre-pass + `skip_if_clean`.
- Translation: `llama_translate` = Hy-MT2 1.8B (default) or PLaMo Q4_K_M (swap + `template_style: plamo`).
- TTS: Kokoro (en) / piper-ru (ru) auto; cloning stays on qwen3-tts-clone GPU helper (idle-stops 120 s).
- Engines: audio.cpp only; NeMo-Speech.cpp and omnivoice.cpp external.
- Tests: `python -m pytest tests/ -q` -> **165 passed**.

## Unresolved blockers

1. OmniVoice in audio.cpp needs a packed GGUF; stock ggml rejects the split tokenizer's long tensor names.
2. OmniVoice weights CC-BY-NC — not distributable.
3. CUDA source builds: the CUDA **toolkit** was missing (only the driver was present);
   nvcc 12.9.86 has now been installed from NVIDIA's public redist into
   `runtime/cuda-toolkit/root` (`nvcc --version` works). The NeMo-Speech.cpp CUDA
   source build was started against it and was in the vcpkg/compile phase at the end
   of this session (log: `runtime/logs/nemo-cuda-build2.log`). Prebuilt GPU binaries
   (`llama-cuda`, `audiocpp-cuda`) always worked — the settings panel's GPU option is
   verified functional again after `llama-cuda` was restored.
4. PLaMo vs Hy-MT2 latency under idle load not re-measured; bench-case quality is a tie.
