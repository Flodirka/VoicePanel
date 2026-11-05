# Models: what runs where, tested variants, and how to swap them

VoicePanel ships no weights. The service reads every model from `config.yaml`
(`config.example.yaml` is the annotated template), so replacing any component
is a config edit — the panel and the API contract do not change.

## How the pipeline is wired

```text
microphone / loopback audio
   └─ ASR            audiocpp · nemotron (default) or whisper        [main group, CPU/GPU]
   └─ VAD            Silero (panel-side, ONNX)
   └─ text           corrector  llama.cpp · GEC 0.8B                 [llm group, CPU/GPU]
                     translator llama.cpp · PLaMo 3 / Hy-MT2         [llm group]
   └─ TTS built-in   audiocpp · Kokoro (en), Piper ru_RU male/female [main group, CPU]
   └─ TTS clone      omnivoice.cpp · OmniVoice 0.6B Q8_0             [clone group, CPU/GPU]
                     (alternative: audio.cpp · qwen3-tts-clone)
   └─ diarization    audiocpp · nemotron-3-diarization               [diar group, CPU/GPU]
```

Compute placement (CPU vs GPU per group) is switched live from the panel or
`POST /admin/compute`; the choices persist in `runtime/state/compute.json`.
For calls and the live interpreter switch `main`, `llm` and `clone` to the GPU
profile — that is the "everything on CUDA" mode.

## Tested set (2026-11, this project's reference hardware)

All numbers are honest measurements on the reference machine — Ryzen-class
laptop CPU (8 threads, other workloads running concurrently) and RTX 4060
Laptop 8 GB that was ~60 % busy with another CUDA process during the GPU runs.
Treat GPU numbers as pessimistic; an idle card is faster.

| Slot | Model (size on disk) | Engine | Device | Measured | Notes |
|---|---|---|---|---|---|
| ASR | `nemotron-3.5-asr-streaming-0.6b` Q8_0 (931 MB) | audio.cpp | CPU | RTF 0.41–0.46 | fastest bilingual recogniser measured; EN-only punctuation, pair with correction |
| ASR | `whisper small` (488 MB) | whisper.cpp | CPU | slower, timestamps | use when you need segment timestamps |
| VAD | `silero_vad.onnx` (2.3 MB) | ONNX Runtime | CPU | sub-real-time | panel-side segmentation |
| TTS built-in ru | `piper-ru-ruslan-medium` (62 MB, male) | audio.cpp | CPU | RTF 0.45 | real-time on a busy CPU |
| TTS built-in ru | `piper-ru-irina-medium` (62 MB, female) | audio.cpp | CPU | RTF 0.47 | real-time on a busy CPU |
| TTS built-in en | `kokoro-82m` Q8_0 (190 MB) | audio.cpp | CPU | RTF 3.2–4.8 on a loaded CPU | good quality; slow on a busy box — prefer GPU or accept the delay |
| TTS clone | `omnivoice` 0.6B Q8_0 (945 MB: base 656 + tokenizer 289) | omnivoice.cpp | GPU | auto RTF 1.9 (steps 8) / 4.5 (steps 16); clone RTF 6.9 (steps 16) | 600+ languages, zero-shot clone from 5–20 s reference; CC-BY-NC weights |
| TTS clone | same | omnivoice.cpp | CPU | auto RTF 13.1; clone RTF 24.2 (steps 16) | works everywhere, not real-time |
| TTS clone (alt) | `qwen3-tts-12hz-0.6b-base` Q8_0 (2.0 GB) | audio.cpp | GPU | ~6.4 GB VRAM peak | ru/en + 8 languages; needs the audio.cpp clone helper |
| Corrector | `Qwen3.5-0.8B-GEC-KAZ-RUS-ENG` Q4_0 (563 MB) | llama.cpp | CPU | sub-second per clean sentence (skipped entirely when `skip_if_clean` finds nothing) | needs `enable_thinking=false` (the service always sends it) |
| Translator | `plamo-3-translate-0.8b` Q4_K_M (573 MB) | llama.cpp | CPU | 4.3 s warm / ~10 s cold under load | keeps identifiers via glossary; `template_style: plamo` |
| Translator (alt) | `Hy-MT2-1.8B` Q4_K_M (1.1 GB) | llama.cpp | CPU | 0.5 s idle p50 | quality tie with PLaMo in bench cases |
| Diarization | `nemotron-3-diarization` Q8_0 (107 MB) | audio.cpp | CPU | real-time factor well under 1 for short clips | speaker turns for meeting transcripts |

The OmniVoice Q8 clone path was verified end-to-end through the service
(profile preview on the GPU: 5.7 s of cloned audio in ~25 s wall including
engine start and model load; a warm engine skips the load).

## Swapping a model

Every slot is a config block; point `path` at another GGUF and restart the
service (or use the panel's model pickers, which list whatever `/v1/models`
reports).

### Built-in voices (male/female choice)

```yaml
tts:
  default_voices:
    en: ""                  # empty -> kokoro
    ru: piper-ru-female     # or piper-ru (male), or a cloned profile id
```

Clients may also pass `"model": "piper-ru-female"` per request; explicit
request values always win over the defaults.

### Cloning engine

Default: OmniVoice Q8 through omnivoice.cpp (CPU build; the panel's compute
switch swaps to the CUDA build when `runtime/bin/omnivoice-cuda/` exists).

To use the bigger qwen3-tts-clone instead: enable `backends.audiocpp_clone`,
disable `backends.omnivoice`, and set `tts.clone_model: qwen3-tts-clone`.
Existing profiles keep the model they were created with; PATCH
`/v1/voices/{id}` with `model=` to move a profile between engines.

### Translator

Swap `backends.llama_translate.model_path` / `model_alias` and drop
`template_style: plamo` when leaving PLaMo (other chat templates take a normal
system turn). Translation modes route there automatically; correction stays on
`backends.llama`.

### ASR

`asr.default` picks the id (`nemotron` or `whisper`). Models are declared
under `backends.audiocpp.models` with `task: asr`. Models whose family
rejects a `language` request option need `supports_language: false`.

### Bigger models

The service does not cap model sizes — a slot takes whatever GGUF the engine
loads. Practical upper bounds on the reference machine: a 7–8 B Q4 translator
replaces PLaMo if you accept the slower decode; larger ASR/whisper variants
trade real-time factor for accuracy; `ggml-large-v3-turbo-q5_0` is a known-good
whisper upgrade. Keep the GPU groups on one 8 GB card realistic: cloning
(OmniVoice Q8, ~1.5 GB VRAM with cuBLAS) plus a 4–8 B LLM fits; qwen3-tts-clone
(~6.4 GB VRAM peak) wants the card to itself.

## Where to download

| Component | Source |
|---|---|
| audio.cpp engines + model specs | bundled by `tools/download_runtime.py` (SHA-256 checked) |
| OmniVoice GGUFs | HF `Serveurperso/OmniVoice-GGUF` → `omnivoice-base-Q8_0.gguf`, `omnivoice-tokenizer-Q8_0.gguf` |
| omnivoice.cpp engine | build from source (`runtime/bench/omnivoice.cpp`, `buildcuda.cmd` for GPU); see `docs/alternative-runtimes.md` |
| Piper ru voices | HF `rhasspy/piper-voices` (`ru/ru_RU/{ruslan,irina}/medium`) + convert with audio.cpp's `tests/piper_tts/convert_piper_onnx.py` and `audiocpp_gguf` |
| Nemotron ASR/diarization | pulled by `tools/download_runtime.py` / audio.cpp model manager |
| GEC / PLaMo / Hy-MT2 / Kokoro / Whisper | HF links in `config.example.yaml` comments and `tools/download_runtime.py` |

## Licenses

VoicePanel code is MIT. Model weights keep their own terms and are never
redistributed by this repository: OmniVoice is CC-BY-NC 4.0 (personal,
non-commercial use), PLaMo has a community license (check the card before
commercial use), Piper voices are MIT, Nemotron weights follow NVIDIA's Open
Model license, Qwen models are Apache-2.0 / Qwen license — verify each card
before shipping a bundle. `THIRD_PARTY_NOTICES.md` covers code and assets.
