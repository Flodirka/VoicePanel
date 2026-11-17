# Changelog

## 0.2.0 - 2026-11-17

- Accept neutral speech speed for native OmniVoice and expose voice design only for models that support it.
- Select TTS and ASR backends before service startup or save a new choice from Server settings. CPU/GPU follows the selected engine.
- Reuse one native OmniVoice model across voices, languages and design requests. Cache reference codes and release idle processes.
- Update audio.cpp to 0.9.5, llama.cpp to 1.0/b12359, native OmniVoice to a650381 and ONNX Runtime to 1.32.0.
- Use the trained prompt for the default GEC model. Remove the service's automatic output-token cap.
- Retry queued text on the next phrase after a processing failure, without restarting the microphone.
- Read audio.cpp streaming recognition results while audio is still being uploaded.
- Report a reset streaming connection as an engine failure so the session can handle it.
- Package public tracked files and deployed runtime files. Keep local notes, configuration, profiles, SDKs and test output outside release archives.

See [release instructions and known limitations](docs/releases/0.2.0.md).
