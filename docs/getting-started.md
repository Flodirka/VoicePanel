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
# Recognition, built-in English speech, LLM, Whisper and engine variants:
python tools/download_runtime.py --group runtime

# ONNX Runtime C++ SDK and Silero VAD:
python tools/download_runtime.py --group extras

# Optional CUDA voice-cloning helper and its model:
python tools/download_runtime.py --group clone
```

Downloads can be several gigabytes. The URLs and SHA-256 values are pinned in `tools/downloads.lock.json`. Model URLs use immutable repository revisions. Each file must match its locked hash before an archive is extracted. Cached files are verified too; a mismatch stops the process. Extraction rejects paths outside the destination and symlinks. The downloader does not bypass TLS checks, run downloaded binaries, or fetch a script and execute it.

The `--verify-only` option checks files already present without downloading or extracting. The shell scripts remain compatibility entry points for the same verified Python downloader.

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
  audiocpp_clone:
    enabled: false
```

These are edits to the corresponding blocks, not a replacement for the full sample file. Main ASR and built-in speech already use `runtime/bin/audiocpp-cpu/audiocpp_server.exe`. The configured Whisper server is `runtime/bin/whisper-cpu/Release/whisper-server.exe`. Check the extracted archive layout against these paths.

Kokoro may also need eSpeak NG on the system. The sample points to `runtime/third_party/espeak-ng/eSpeak NG/`; install it from [the upstream eSpeak NG project](https://github.com/espeak-ng/espeak-ng) or update `espeak_lib` and `espeak_data` to your installation. The downloader does not install eSpeak NG. Model and voice selection is described in [CPU and GPU configuration](compute-profiles.md).

Optional cloned speech needs the cloning model and a reference recording you have the right to use. The sample cloning helper uses CUDA. Do not enable it until its binaries, runtime libraries and model are installed. Keep personal references under the ignored runtime directory.

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

## 5. First dictation

Open a text field in Notepad. Press Ctrl+Alt+D, speak, pause, and continue. Completed processed text should arrive while capture stays active. Press the shortcut again and wait for the remaining work. Use Ctrl+Alt+R when you want Preview and confirmation before insertion. The shortcuts are configurable in Settings.

Accept and Reject apply to all current corrections in one click. Quick dictation starts a new transcript after the configured silence interval while capture stays open. An insertion error does not prevent continuing or starting another recording. Use History in Preview while stopped to restore earlier text and retry Insert, Copy or Save.

Panel auto-save is enabled by default. It writes stable text and reviewed edits to your selected folder, or to `transcripts` in VoicePanel's local application-data directory when no folder is selected. Settings shows the effective folder. Disable auto-save to prevent automatic transcript-file writes; History then remains in memory until the panel exits.

Review the remembered target and use Copy if the field rejects insertion. A normal application cannot inject input into every elevated or protected field. Do not run VoicePanel as administrator merely to avoid that boundary.

## Network and privacy

Read [Security](../SECURITY.md). Origin and Host checks cover HTTP and WebSocket traffic. Browser origins are denied unless explicitly allowlisted. Optional bearer authentication covers both transports. The standard launcher refuses non-loopback binding without authentication and rejects wildcard binds. The panel has no bearer-token setting, so the normal panel setup uses loopback with no service token.

External LLM configuration is optional. Enabling it sends text and context to the selected endpoint. Local audio, references and Markdown records are never part of the public source repository.
