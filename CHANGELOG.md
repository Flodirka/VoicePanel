# Changelog

## Unreleased

- Label speakers in the subtitles overlay. Captioned application audio runs through the same rolling diarisation window as a meeting, so captions say who spoke.
- Keep speaker numbers stable across a call. Diariser and known turns are compared on one session clock, and a voice inherits a person only when the overlap is a real share of its speech; a boundary sliver used to renumber voices or merge them onto one label.
- Keep every voice on its own label when a recording has more than two. Each person's known speech is stored merged, so the turns a window reports again cannot outvote the voice that is actually speaking; the long caption acceptance run now scores every phrase against a whole-recording reference diarisation and requires one label per voice.
- Draw transcript-only captions at the translation size after a text-size change, and keep their line height in step with the font.
- Score the long caption acceptance on the final transcript, not only on labels and latency. The report adds WER and CER against a human transcript, covered speech, lost turns, phrases that came back without words, speaker-name accuracy and term recall, and fails when the live run drops too much of the room.
- Count a lost turn by its words instead of by timing. Captions sit wall to wall, so the old overlap test reported zero lost turns on every run, including the one right before it. A turn now counts as lost when fewer than half of its words appear in the captions over its span, and the report also carries the share of checked words that no caption echoes.
- Add an offline re-pass for a saved recording: whole-file diarisation, re-recognition per diarised turn and per VAD phrase, cleanup with meeting notes and the context document, and a named Markdown transcript. It reports its own numbers next to the live run's.

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
