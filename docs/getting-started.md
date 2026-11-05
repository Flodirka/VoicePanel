# Getting started

VoicePanel is a source distribution for Windows 11 x64. Install the development tools first, then download the selected runtime files. Nothing in the runtime downloader executes downloaded programs or installs Python packages.

## 1. Development tools

Install Python 3.11 or 3.12, Git, Visual Studio 2022 with **Desktop development with C++**, CMake and Ninja. Install Qt **6.8.3**, target **msvc2022_64**, from the [official Qt installer](https://www.qt.io/download-qt-installer-oss). The build script expects it at `runtime/Qt/6.8.3/msvc2022_64`; its `-QtDir` option accepts another installation path.

```powershell
git clone https://github.com/Flodirka/VoicePanel.git
cd VoicePanel
python -m pip install --only-binary=:all: --require-hashes -r service/requirements.lock
```

The Python dependencies, inference engines and model weights are separate third-party components. Review [Third-party notices](../THIRD_PARTY_NOTICES.md) and their upstream terms.

## 2. Verified runtime downloads

```powershell
# Recognition, built-in speech, LLM, Whisper and engine variants:
python tools/download_runtime.py --group runtime

# ONNX Runtime C++ SDK and Silero VAD:
python tools/download_runtime.py --group extras

# Optional: the audio.cpp CUDA build and the qwen3-tts-clone alternative model:
python tools/download_runtime.py --group clone
```

Downloads can be several gigabytes. The URLs and SHA-256 values are pinned in `tools/downloads.lock.json`. Model URLs use immutable repository revisions. Each file must match its locked hash before an archive is extracted. Cached files are verified too; a mismatch stops the process. Extraction rejects paths outside the destination and symlinks. The downloader does not bypass TLS checks, run downloaded binaries, or fetch a script and execute it.

The `--verify-only` option checks files already present without downloading or extracting. The shell scripts remain compatibility entry points for the same verified Python downloader.

The default **cloning engine** is not part of the locked downloads: it is the
k2-fsa omnivoice.cpp engine built from source (see
[Models](models.md#where-to-download)), driving the OmniVoice Q8 weights you
download yourself from Hugging Face into `runtime/models/omnivoice/`
(`omnivoice-base-Q8_0.gguf` + `omnivoice-tokenizer-Q8_0.gguf`, ~945 MB total,
CC-BY-NC weights). The CPU build is enough to try cloning; the CUDA build
(runtime/bin/omnivoice-cuda, plus cuBLAS/cudart runtime DLLs) is the
real-time option. The downloader's `--group clone` installs the audio.cpp
alternative instead.

## 3. Local configuration

```powershell
Copy-Item config.example.yaml config.yaml
```

Keep the service host at `127.0.0.1`. For a CPU-only setup, set these values in the copied configuration:

```yaml
backends:
  llama:
    binary: runtime/bin/llama-cpu/llama-server.exe
    variant_dirs:
      - runtime/bin/llama-cpu
    gpu_layers: 0
  omnivoice:
    binary: runtime/bin/omnivoice-cpu/omnivoice-tts.exe
    codec_binary: runtime/bin/omnivoice-cpu/omnivoice-codec.exe
    backend: cpu
```

These are edits to the corresponding blocks, not a replacement for the full sample file. Main ASR and built-in speech already use `runtime/bin/audiocpp-cpu/audiocpp_server.exe`. The configured Whisper server is `runtime/bin/whisper-cpu/Release/whisper-server.exe`. Check the extracted archive layout against these paths.

Kokoro may also need eSpeak NG on the system. The sample points to `runtime/third_party/espeak-ng/eSpeak NG/`; install it from [the upstream eSpeak NG project](https://github.com/espeak-ng/espeak-ng) or update `espeak_lib` and `espeak_data` to your installation. The downloader does not install eSpeak NG. Model and voice selection is described in [CPU and GPU configuration](compute-profiles.md).

Optional cloned speech needs the cloning engine (omnivoice.cpp + the OmniVoice weights you download; see [Models](models.md)) and a reference recording you have the right to use. The CPU engine build works everywhere; the CUDA build is the real-time option and needs the cuBLAS/cudart runtime DLLs beside it. Keep personal references under the ignored runtime directory.

## 4. Build and start

```powershell
.\tools\build_panel.ps1
.\tools\start-service.ps1
.\tools\start-panel.ps1
```

For a different Qt installation:

```powershell
.\tools\build_panel.ps1 -QtDir 'C:\Qt\6.8.3\msvc2022_64'
```

The executable is written to `panel/build/bin/VoicePanel.exe`. The build script deploys Qt runtime files and copies the VAD model beside the executable. Keep those files together when running the panel.

Check `http://127.0.0.1:8080/health` for service health. A healthy gateway does not mean every model is installed or loaded. Backend logs remain under `runtime/logs/` on your machine.

## 4a. Distributable bundle and in-workflow verification

```powershell
.\tools\package_release.ps1 -CpuOnly                 # dist\VoicePanel, CPU-only engines and models
.\tools\package_release.ps1 -Zip                     # also dist\VoicePanel-<version>.zip + .sha256
```

The bundle is one self-contained folder: `VoicePanel.exe` with the Qt runtime beside it, `service/`, `tools/`, `runtime/` (engine binaries, models, eSpeak/ONNX), `config.example.yaml`, and the licenses. Copy `config.example.yaml` to `config.yaml` and edit it inside the bundle.

Two checks run against the bundle as shipped:

```powershell
cd dist\VoicePanel
.\VoicePanel.exe --check-bundle        # PASS/FAIL per part, written to check-bundle.txt; never opens the mic
python tools\run_service.py            # start the bundled service
python tools\verify_workflow.py --base http://127.0.0.1:8080 --audio my-recording.wav `
    --dictionary my-terms.txt        # one term per line; optional --context context.md
```

`verify_workflow.py` replays the panel's real dictation chain — WAV to `/v1/audio/transcriptions`, sentence split, then `/v1/text/process` per sentence with the user dictionary — so the deterministic pre-pass (near-miss fixes of terminology, reported in `notes`) and the `llm.skip_if_clean` gate (clean sentences return with `model: null` and no model call) are exercised on real recogniser output, not hand-written text.

## 5. First dictation

Open a text field in Notepad. Press Ctrl+Alt+D, speak, pause, and continue. Completed processed text should arrive while capture stays active. Press the shortcut again and wait for the remaining work. Use Ctrl+Alt+R when you want Preview and confirmation before insertion. The shortcuts are configurable in Settings.

Accept and Reject apply to all current corrections in one click. Quick dictation starts a new transcript after the configured silence interval while capture stays open. An insertion error does not prevent continuing or starting another recording. Use History in Preview while stopped to restore earlier text and retry Insert, Copy or Save.

Panel auto-save is enabled by default. It writes stable text and reviewed edits to your selected folder, or to `transcripts` in VoicePanel's local application-data directory when no folder is selected. Settings shows the effective folder. Disable auto-save to prevent automatic transcript-file writes; History then remains in memory until the panel exits.

Review the remembered target and use Copy if the field rejects insertion. A normal application cannot inject input into every elevated or protected field. Do not run VoicePanel as administrator merely to avoid that boundary.

## 6. Sending the translated voice into a call (optional)

Listening and dictation need no extra software. VoicePanel captures a call per
application (Windows 10 2004 or newer) or per output device, and types the text
into the focused field.

Sending your **translated voice** to another application is different. A capture
endpoint that other programs can select as a microphone can only be created by
an audio driver, so VoicePanel cannot provide one from user mode. Use a virtual
audio cable:

1. Get a virtual cable. Check the Windows **Recording** tab first: audio
   interfaces often install a pair already (`Chat Input (Maono Arena)` next to
   `Chat Output`, `Stream Input` next to `Virtual Output`). If nothing pairs
   with a playback endpoint, install [VB-CABLE](https://vb-audio.com/Cable/)
   (free driver; restart Windows once) which adds `CABLE Input` and
   `CABLE Output`.
2. In VoicePanel, open **Live**, expand **Settings**, and set
   **My voice goes to** to the cable's playback endpoint (`Chat Output
   (Maono Arena)`, `CABLE Input`, …).
3. In the call or recorder application, select the matching recording endpoint
   (`Chat Input (Maono Arena)`, `CABLE Output`, …) as its microphone. That
   application now hears the synthesized voice.
4. Leave **My microphone** on the real microphone, and keep **Call audio** on
   the application or device that plays the call. Do not route the call's own
   output back into the cable, or the far end will hear itself.

VoicePanel does not bundle or redistribute VB-CABLE; its terms are VB-Audio's.
Any other virtual cable behaves the same way (VoiceMeeter, Virtual Audio Cable,
or the endpoints some audio interfaces install), as long as it exposes a
playback endpoint and a matching recording endpoint.

To check the routing before a call, press **Preview**: it speaks one sentence
into the selected device, so `CABLE Output` should show a level in the other
application.

## Network and privacy

Read [Security](../SECURITY.md). Origin and Host checks cover HTTP and WebSocket traffic. Browser origins are denied unless explicitly allowlisted. Optional bearer authentication covers both transports. The standard launcher refuses non-loopback binding without authentication and rejects wildcard binds. The panel has no bearer-token setting, so the normal panel setup uses loopback with no service token.

External LLM configuration is optional. Enabling it sends text and context to the selected endpoint. Local audio, references and Markdown records are never part of the public source repository.
