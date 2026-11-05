# CPU and GPU configuration

VoicePanel routes work to separate supervised inference processes. Settings in the server tray can select available devices for the main speech engine, cloned speech, speaker separation and text processing. Choices are persisted locally in `runtime/state/compute.json` and applied when the corresponding process is idle.

| Work | Process | Configuration |
|---|---|---|
| Main ASR and built-in speech | audio.cpp | `backends.audiocpp.binary`, `backend`, `device` |
| Voice cloning (default) | omnivoice.cpp CLI | `backends.omnivoice.binary`, `codec_binary`, `backend` |
| Voice cloning (alternative) | separate audio.cpp helper | `backends.audiocpp_clone.enabled`, `binary`, `backend`, `device` |
| Correction and translation | llama.cpp | `backends.llama.binary`, `gpu_layers` |
| Speaker separation | separate audio.cpp helper | `backends.audiocpp_diar.binary`, `backend`, `device` |
| Optional timestamped ASR | whisper.cpp | `backends.whisper.binary`; the prepared build is CPU-only |

ASR and built-in TTS models inside one audio.cpp process share that process's device. They cannot be assigned different devices independently. The cloning engine is its own process, so main speech can stay on CPU while cloned speech uses the GPU.

## CPU-only setup

Edit these fields in the copied `config.yaml`:

```yaml
backends:
  audiocpp:
    binary: runtime/bin/audiocpp-cpu/audiocpp_server.exe
    backend: cpu
  omnivoice:
    binary: runtime/bin/omnivoice-cpu/omnivoice-tts.exe
    codec_binary: runtime/bin/omnivoice-cpu/omnivoice-codec.exe
    backend: cpu
  llama:
    binary: runtime/bin/llama-cpu/llama-server.exe
    variant_dirs:
      - runtime/bin/llama-cpu
    gpu_layers: 0
  whisper:
    binary: runtime/bin/whisper-cpu/Release/whisper-server.exe
```

Keep the rest of the sample configuration, including model paths and ports. The CPU omnivoice build works everywhere but is not real-time (measured RTF 13–24 under load). Built-in speech (Piper for Russian, Kokoro for English) stays real-time on CPU.

## Everything on CUDA (calls and the live interpreter)

Switching the `main`, `clone`, `diar` and `llm` groups to the GPU profile from
the panel (or `POST /admin/compute`) is the configuration for real-time work
during calls:

* `runtime/bin/audiocpp-cuda/` serves recognition on the GPU.
* `runtime/bin/omnivoice-cuda/` runs the OmniVoice cloner on the GPU
  (measured RTF 1.9–6.9 on a partially busy RTX 4060). The directory needs
  `cublas64_12.dll`, `cublasLt64_12.dll`, `cudart64_12.dll` beside the engine —
  the CUDA toolkit parts come from the NVIDIA `nvidia-cublas-cu12` /
  `nvidia-cuda-runtime-cu12` wheels or a full CUDA 12 install.
* llama.cpp's CUDA build takes the text models with `gpu_layers: 99`.

Availability is detected from the binaries on disk: a group only offers `gpu`
when its CUDA build exists, and switching refuses (`409`) while that process
serves a request.

## Cloned speech

A reference profile contains an audio file and transcript. Creating a profile does not train a model. The OmniVoice engine pre-encodes each profile's reference once into a tiny `.rvq` beside the WAV, then synthesizes with it; a voice switch restarts the engine with that reference. Record or import only voices you have permission to use. Reference files remain under the ignored runtime directory.

The engine process starts on demand and stops after its configured idle period (`backends.omnivoice.idle_stop_s`), releasing ~1 GB of RAM or the corresponding VRAM on the CUDA build.

The qwen3-tts-clone alternative (audio.cpp helper, 2 GB model, ru/en + 8 languages, ~6.4 GB VRAM peak) is configured in the sample but disabled; see [Models](models.md#cloning-engine) for the swap.

## Text processing

`gpu_layers: 0` selects CPU inference. A compatible CUDA or Vulkan build can offload model layers when `gpu_layers` is increased. Choose the corresponding binary and runtime libraries explicitly. The `variant_dirs` list controls fallback executable discovery; keep it to the variants you intend to use.

An existing text server can be configured with `backends.llama.external: true` and its host and port. The gateway then uses that server without starting or stopping it. If the endpoint is on another machine, text and context leave the local machine. Keep the chosen endpoint on a trusted network.

## Switching while work is pending

The compute API rejects a change with `409` when the affected process is busy. It does not stop a model underneath an active request. Device availability depends on which verified runtime variants are present. Copying source files alone does not install GPU drivers or runtime libraries.

Health, capabilities and compute endpoints are listed in the [API reference](api.md). Read [Getting started](getting-started.md) for downloads and [Security](../SECURITY.md) for network boundaries.
