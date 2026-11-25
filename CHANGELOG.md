# Changelog

## 0.3.2 - 2026-11-25

- Fold an empty subtitle phrase into the next caption instead of giving it a line of its own. The fragment's audio is held and recognised with the next phrase (bounded to 3 s), so two empties in a row reach the recogniser as one request and a fragment at the end of a session is dropped. No extra request is made.
- Measured on the 600-second three-voice fixture: WER 0.303 → 0.285, lost turns 20.6 % → 14.4 %, term recall 79.5 % → 82.1 %, speaker names 92.3 % → 94.0–95.7 %, empty lines 20 → 0, caption order 0/0, median latency 186 → 186 ms.
- Cover the merge model-free: two empty phrases in a row plus a fragment at the end of the session, checking the folded audio, the counters, and that the trailing fragment never becomes a caption.

See [release notes and known limitations](docs/releases/0.3.2.md).

## 0.3.1 - 2026-11-25

- Show a subtitle line as soon as its words are known. The rolling diarisation window answers seconds later, so its label corrects that phrase by id instead of holding the text; until then the line carries the last resolved label.
- Match a window turn to a known person on the session clock with mutual overlap, and settle the ids left over by elimination. A voice the engine re-cut can no longer mint a second label, and a window that overlaps no labelled speech names nobody.
- Continue diarisation from the last labelled second in half-window steps when the engine falls behind the audio. Jumping to the newest audio left the speech in between unlabelled.
- Add a single-voice acceptance run: replay a real one-speaker recording and fail if the rolling window invents a second participant.
- Explain a report with `transcript_quality.py diagnose`: every empty phrase by length, loudness, voices and distance to a turn change; every lost turn charged to `not_cut`, `empty_phrase` or `partial`; repeated offsets counted once.
- Keep the recognition `prompt` in `verbose_json` transcriptions. The detail variant used to take none, so a glossary sent with that format was dropped without an error.
- Remember the subtitles overlay size between sessions.
- Remove the empty-phrase recovery experiment. It filled 10 of 20 empty phrases with `P`/`Puh.` and only added two requests per empty phrase; with a plausibility gate the run was the baseline (14.6 % empty, 20.6 % lost, WER 0.303). The fragment is the tail of a word whose beginning is already captioned.

See [release notes and known limitations](docs/releases/0.3.1.md).

## 0.3.0 - 2026-11-21

- Label speakers in the subtitles overlay. Captioned application audio runs through the same rolling diarisation window as a meeting, so captions say who spoke.
- Keep speaker numbers stable across a call. Diariser and known turns are compared on one session clock, and a voice inherits a person only when the overlap is a real share of its speech; a boundary sliver used to renumber voices or merge them onto one label.
- Keep every voice on its own label when a recording has more than two. Each person's known speech is stored merged, so the turns a window reports again cannot outvote the voice that is actually speaking; the long caption acceptance run now scores every phrase against a whole-recording reference diarisation and requires one label per voice.
- Draw transcript-only captions at the translation size after a text-size change, and keep their line height in step with the font.
- Score the long caption acceptance on the final transcript, not only on labels and latency. The report adds WER and CER against a human transcript, covered speech, lost turns, phrases that came back without words, speaker-name accuracy and term recall, and fails when the live run drops too much of the room.
- Count a lost turn by its words instead of by timing. Captions sit wall to wall, so the old overlap test reported zero lost turns on every run, including the one right before it. A turn now counts as lost when fewer than half of its words appear in the captions over its span, and the report also carries the share of checked words that no caption echoes.
- Add an offline re-pass for a saved recording: whole-file diarisation, re-recognition per diarised turn and per VAD phrase, cleanup with meeting notes and the context document, and a named Markdown transcript. It reports its own numbers next to the live run's.

See [release notes and known limitations](docs/releases/0.3.0.md).

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
