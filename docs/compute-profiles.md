# CPU and GPU configuration

VoicePanel routes work to separate supervised inference processes. Settings in the server tray can select available devices for the main speech engine, cloned speech and text processing. Choices are persisted locally in `runtime/state/compute.json` and applied when the corresponding process is idle.

| Work | Process | Configuration |
|---|---|---|
| Main ASR and built-in speech | audio.cpp | `backends.audiocpp.binary`, `backend`, `device` |
| Optional voice cloning | Separate audio.cpp helper | `backends.audiocpp_clone.enabled`, `binary`, `backend`, `device` |
| Correction and translation | llama.cpp | `backends.llama.binary`, `gpu_layers` |
| Optional timestamped ASR | whisper.cpp | `backends.whisper.binary`; the prepared build is CPU-only |

ASR and TTS models inside one audio.cpp process share that process's device. They cannot be assigned different devices independently. The separate cloning helper makes it possible to keep main speech on CPU and run cloned speech on GPU.

## CPU-only setup

Edit these fields in the copied `config.yaml`:

```yaml
backends:
  audiocpp:
    binary: runtime/bin/audiocpp-cpu/audiocpp_server.exe
    backend: cpu
  audiocpp_clone:
    enabled: false
  llama:
    binary: runtime/bin/llama-cpu/llama-server.exe
    variant_dirs:
      - runtime/bin/llama-cpu
    gpu_layers: 0
  whisper:
    binary: runtime/bin/whisper-cpu/Release/whisper-server.exe
```

Keep the rest of the sample configuration, including model paths and ports. Disabling the optional clone helper prevents it from starting a CUDA process. English built-in speech remains available when its model and dependencies are installed. Cloned speech needs a configured cloning model and a reference profile.

## Optional cloned speech on CUDA

Run the verified downloader with `--group clone`, then enable the helper in `config.yaml`. Its default port is `8104`, separate from the main audio.cpp process on `8101`. The helper uses `runtime/bin/audiocpp-cuda/audiocpp_server.exe` and the Qwen3-TTS Base model listed in the sample.

A reference profile contains an audio file and transcript. Creating a profile does not train a model. Speech synthesis runs inference using that reference. Record or import only voices you have permission to use. Reference files remain under the ignored runtime directory.

The helper starts on demand and stops after its configured idle period. This releases its process resources. Actual memory use and speed depend on the selected model, engine build and hardware.

## Text processing

`gpu_layers: 0` selects CPU inference. A compatible CUDA or Vulkan build can offload model layers when `gpu_layers` is increased. Choose the corresponding binary and runtime libraries explicitly. The `variant_dirs` list controls fallback executable discovery; keep it to the variants you intend to use.

An existing text server can be configured with `backends.llama.external: true` and its host and port. The gateway then uses that server without starting or stopping it. If the endpoint is on another machine, text and context leave the local machine. Keep the chosen endpoint on a trusted network.

## Switching while work is pending

The compute API rejects a change with `409` when the affected process is busy. It does not stop a model underneath an active request. Device availability depends on which verified runtime variants are present. Copying source files alone does not install GPU drivers or runtime libraries.

Health, capabilities and compute endpoints are listed in the [API reference](api.md). Read [Getting started](getting-started.md) for downloads and [Security](../SECURITY.md) for network boundaries.
