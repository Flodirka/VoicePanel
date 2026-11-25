# Testing

The repository separates model-free service tests from Windows desktop and real-model checks. Use synthetic text and generated audio in automated fixtures. Do not add personal recordings, private configuration, transcripts or model files to Git.

## Service tests

```powershell
python -m pip install --only-binary=:all: --require-hashes -r service/requirements.lock
python -m pip install pytest==9.1.1
python -m pytest tests -q
```

These tests use temporary directories, synthetic audio and controlled backend responses. They cover configuration, voice-profile storage, routing, text processing, compute choices, recognition context in every response format, HTTP/WebSocket security, and download integrity and archive extraction.

## Panel tests

Prepare the dependencies in [Getting started](getting-started.md), then run from a Visual Studio developer shell:

```powershell
.\tools\build_panel.ps1
.\tools\run_panel_tests.ps1
```

The runner puts the suite under a hard budget instead of letting it compete with the desktop: one process at a time (a second run is refused), one logical core (`-MaxCores`), below-normal priority (`-Priority`), a memory cap enforced by a Job Object (`-MemoryLimitMB`, default 768), a wall-clock timeout (`-TimeoutSeconds`) and a per-test timeout (`-FunctionTimeoutSeconds`, exported as `QTEST_FUNCTION_TIMEOUT`). A full run therefore takes longer than the raw executable; it prints the peak working set and CPU time next to the QtTest totals, so the budget can be checked against what the panel itself uses. If a run dies at the cap, raise `-MemoryLimitMB` and record the new peak; do not drop the cap.

`-Filter <name>` runs named tests only, `-LogPath` moves the report (default `runtime/logs/panel-tests.txt`), and `-Native` switches from the offscreen platform to the windows one for desktop checks. `-SelfTest` proves the cap is real: a 512 MB allocation must fail inside a 128 MB job. `ctest --test-dir panel/build --output-on-failure` remains available for the quick offscreen pass, but it has no memory or CPU cap.

Headless tests skip native microphone and desktop input checks. For native coverage the runner's `-Native` flag adds the platform; the test preferences use temporary storage.

## Manual desktop acceptance

1. Focus a text field in Notepad and start Quick Dictation with Ctrl+Alt+D.
2. Speak several sentences with pauses. Check that completed text arrives in order, with no partial-result insertion and no microphone restart between sentences.
3. Stop while recognition or text processing is pending. Press the hotkeys again during finalization. Check that recording does not restart and text is not duplicated.
4. Start a second session in the same field. Check spacing and that earlier text is preserved.
5. Use Ctrl+Alt+R. Check that Preview opens after pending work finishes and that no text enters the target until Insert is confirmed.
6. Make the target unavailable and attempt insertion. Start another recording without pressing Copy. Check that capture starts, the earlier text remains in History, and restoring it while stopped keeps the original insertion progress. Restore the target and retry; only the uninserted part should be sent.
7. Test a silence reset longer than the processing time, then shorter than the processing time. Original text and translations must remain available in History. Repeat with storage disabled and with an unwritable output folder. Neither failure should lock capture or prevent completed quick documents from moving to History.
8. Check that both the toolbar and Preview are frameless.
9. Check auto-save before Stop and without insertion. Verify original text, completed results and reviewed edits in the effective transcript folder. Test enabled auto-save with no custom folder, then disable auto-save and confirm that automatic writes stop.
10. Produce several corrections. One Accept or Reject click must resolve all current corrections. Later results must preserve that decision and manual edits.

Repeat insertion checks in the applications you intend to use. Notepad coverage does not establish compatibility with every application or protected field.

The opt-in `desktopInsertionIntoFocusedTarget` test uses `VOICEPANEL_TARGET_TEST=1` and a unique local file path in `VOICEPANEL_TARGET_CONTINUE`. Focus the intended field and press Ctrl+Alt+D. Press it again when `target-insertion-stage.txt` says `stop-pending`. At `verify-target`, inspect the actual application field for both sentences in order, then create the confirmation file. The test must not be counted as a pass from its internal insertion state alone. Use a dedicated draft and do not submit messages. Keep the confirmation file and logs local.

## Real-model checks

Start the configured local service and use `tools/verify_service.py` with a synthetic or explicitly approved reference recording. Its output stays under the ignored runtime directory. Do not upload generated reports or audio by default.

For repeatable speed, resource and quality numbers, use `tools/bench/run_bench.py`. It measures ASR (WER, CER, latency, real-time factor, partial results), correction and translation on the synthetic suites in `tools/bench/cases/`, a request-level chain (one ASR request plus one text request, cold, with warm-up, and steady state), KV prefix reuse from llama-server's own counters, and speech synthesis, and samples memory, CPU and GPU of every engine process. Its text suites can also go straight to a llama-server to compare candidate models or the prompt order before the audit. These are request-level figures, not the latency a user sees.

For user-facing latency (speech end to inserted text), start the panel with `VOICEPANEL_TIMING_LOG` set to a file path, dictate with the protocol in [Performance audit](performance-audit.md#panel-end-to-end-latency-windows), and analyze the log with `tools/bench/panel_timing.py`. Without the variable the panel writes nothing. Commands and acceptance criteria are in [Performance audit](performance-audit.md#9-local-benchmark-instructions). Results stay under `runtime/logs/bench/`.

The panel's opt-in inference test needs a local `live-fixture.wav` in `panel/build`. The interactive dictation test needs a dedicated Notepad document and explicit test environment variables documented in its source. Those tests are skipped in normal model-free CI. Keep their logs and fixtures local.

The long meeting and caption acceptance runs replay local fixtures against a running service. From the repository root, with the service on its default address:

```powershell
$env:VOICEPANEL_MEETING_BENCH = "http://127.0.0.1:8080"
.\tools\run_panel_tests.ps1 -Filter meetingLongRecordingKeepsSpeakersAndLatency `
  -TimeoutSeconds 3600 -FunctionTimeoutSeconds 1200 -LogPath runtime\logs\meeting-long.txt
$env:VOICEPANEL_SUBTITLES_BENCH = "http://127.0.0.1:8080"
.\tools\run_panel_tests.ps1 -Filter subtitlesLongRecordingKeepsSpeakersAndLatency `
  -TimeoutSeconds 3600 -FunctionTimeoutSeconds 1200 -LogPath runtime\logs\subtitles-long.txt
```

Run the service at below-normal priority too (`(Start-Process python -ArgumentList 'tools\run_service.py','--config','config.yaml' -PassThru).PriorityClass = 'BelowNormal'`), so the acceptance does not compete with the desktop either. They need `runtime/bench/meeting-long-16k.wav` and `runtime/bench/meeting-3voices-16k.wav` and take several minutes each. Each writes its per-phrase report under `runtime/reports/`. Without the fixture or the variable the test is skipped, not passed.

The caption run first diarises the whole fixture in one request and uses that as the acceptance reference. Every phrase is then scored against the voice the reference says is speaking; the test fails unless the three voices land on three distinct labels, each voice keeps its label on at least 80 % of its captioned phrases, no label first appears after the first third, and per-phrase latency does not grow. `runtime/reports/subtitles-long/result.json` records the voice-to-label counts next to the per-phrase timings.

The opposite direction is checked on one real voice: `singleVoiceRecordingKeepsOneLabel` replays `runtime/bench/single-voice-16k.wav` (a real recording of one person, looped to about 2.5 minutes) and fails if the recording gets more than one label, if more than a fifth of its captions are unlabelled, or if per-phrase latency grows. A fixture with three voices can only prove that people are separated; only a single-speaker recording proves the rolling window does not invent participants. Enable it with `VOICEPANEL_SUBTITLES_SINGLE_BENCH` set to the service URL; its report is `runtime/reports/subtitles-single/result.json`.

The merge is covered without a model: `emptyPhrasesFoldIntoTheNextCaptionAndTrailingFragmentIsDropped` feeds two empty phrases in a row, then a phrase with words, then a fragment at the end of the session through the local test server. It checks that the third request carries all three phrases' audio, that the fragments are counted as folded, and that the trailing fragment is held but never captioned and is cleared on stop.

### Transcript quality

The same run scores the words against a human transcript. `runtime/bench/meeting-3voices-ref.json` is the AMI `ami_en2002d_2132` word-level annotation looped with the fixture audio (60 s clip, 61.2 s period); build it once with:

```powershell
python tools\bench\transcript_quality.py reference `
  --annotation runtime\bench\nemo-src\test_files\diar\ami_en2002d_2132.json `
  --period 61.2 --duration 600 --out runtime\bench\meeting-3voices-ref.json
```

The report's `quality` block carries WER, CER, the share of the room's speech that reached a caption, lost human turns, phrases that came back without words, speaker-name accuracy (best label-to-person mapping) and term recall (the user's glossary words the recording contains plus its own frequent content words). A turn is lost when fewer than half of its words appear in the captions over its span (`lost_phrases.recall_threshold`); timing overlap alone would always read zero, because captions sit wall to wall. `missing_word_share` reports the share of checked words no caption echoes. The test fails below 60 % covered speech, above 30 % lost turns, above 25 % empty captions, or below 80 % correct names. Per-phrase rows also carry `start_s`, `end_s` and the recognised `text`, so a report can be scored again without rerunning the audio:

```powershell
python tools\bench\transcript_quality.py score --report runtime\reports\subtitles-long\result.json
```

`diagnose` explains a report instead of restating it, which is where the next round of work starts. Every empty phrase is described by its length, its loudness against the phrases that did carry words, the human voices inside it and how close it starts to a turn change; every lost turn is charged to `not_cut` (no phrase covered it), `empty_phrase` (the phrases over it came back without words) or `partial` (some words, under half). Because the fixture loops the same 60 s clip, the output also counts distinct offsets, so one reproducible failure is not read as twenty random ones:

```powershell
python tools\bench\transcript_quality.py diagnose --report runtime\reports\subtitles-long\result.json
```

It writes `diagnosis.json` and `diagnosis.md` next to the report. The causes are read from the annotation and the saved audio, not from the service, so a report can be diagnosed after the run without the engines loaded.

### Offline re-pass

`offline` reworks a saved recording: one whole-file diarisation request, re-recognition, a cleanup pass with meeting notes and the context document, and a Markdown transcript with speaker names (replaced in code, not by the model). It prints the same metrics for the live run and for the offline reading, and writes `offline.json` and `final.md` under `runtime/reports/subtitles-long/`:

```powershell
python tools\bench\transcript_quality.py offline `
  --wav runtime\bench\meeting-3voices-16k.wav `
  --reference runtime\bench\meeting-3voices-ref.json `
  --live runtime\reports\subtitles-long\result.json `
  --notes runtime\bench\meeting-notes.md `
  --names "speaker_0=Лёша,speaker_1=Женя,speaker_2=Nik" `
  --out runtime\reports\subtitles-long
```

The pass measures two readings of the same audio, one ASR request per diarised turn and one per contiguous 12 s block, then the cleaned paragraphs, so short-window ASR loss and cleanup's effect stay separate numbers. Cleanup runs the model on every paragraph only when the service config sets `llm.skip_if_clean: false`; with the default, a paragraph without a dictionary or spell-checker hit comes back untouched and the tool prints how many were skipped instead of reporting a cleanup. `--source turns|blocks` picks which reading becomes the document: per turn keeps names sharp, while a block is labelled with one person even when two speak inside it. `--cache` reuses recognised chunks under `runtime/bench/`, so changing the cleanup or the notes does not re-run recognition.
