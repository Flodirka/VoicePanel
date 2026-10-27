# Testing

The repository separates model-free service tests from Windows desktop and real-model checks. Use synthetic text and generated audio in automated fixtures. Do not add personal recordings, private configuration, transcripts or model files to Git.

## Service tests

```powershell
python -m pip install --only-binary=:all: --require-hashes -r service/requirements.lock
python -m pip install pytest==9.1.1
python -m pytest tests -q
```

These tests use temporary directories, synthetic audio and controlled backend responses. They cover configuration, voice-profile storage, routing, text processing, compute choices, HTTP/WebSocket security, and download integrity and archive extraction.

## Panel tests

Prepare the dependencies in [Getting started](getting-started.md), then run from a Visual Studio developer shell:

```powershell
.\tools\build_panel.ps1
ctest --test-dir panel/build --output-on-failure
```

Headless tests skip native microphone and desktop input checks. For native coverage, add your Qt `bin` directory to PATH, set `QT_QPA_PLATFORM_PLUGIN_PATH` to its `plugins/platforms` directory, and run `panel/build/bin/VoicePanelTests.exe -platform windows` from `panel/build`. The test preferences use temporary storage.

## Manual desktop acceptance

1. Focus a text field in Notepad and start Quick Dictation with Ctrl+Alt+D.
2. Speak several sentences with pauses. Check that completed text arrives in order, with no partial-result insertion and no microphone restart between sentences.
3. Stop while recognition or text processing is pending. Press the hotkeys again during finalization. Check that recording does not restart and text is not duplicated.
4. Start a second session in the same field. Check spacing and that earlier text is preserved.
5. Use Ctrl+Alt+R. Check that Preview opens after pending work finishes and that no text enters the target until Insert is confirmed.
6. Make the target unavailable, attempt insertion, restore the target and retry. Check that processed text is retained and only the uninserted part is sent.
7. Test a silence reset longer than the processing time, then shorter than the processing time. Original text and translations must remain available. Disable transcript storage and repeat.
8. Check that both the toolbar and Preview are frameless.

Repeat insertion checks in the applications you intend to use. Notepad coverage does not establish compatibility with every application or protected field.

## Real-model checks

Start the configured local service and use `tools/verify_service.py` with a synthetic or explicitly approved reference recording. Its output stays under the ignored runtime directory. Do not upload generated reports or audio by default.

The panel's opt-in inference test needs a local `live-fixture.wav` in `panel/build`. The interactive dictation test needs a dedicated Notepad document and explicit test environment variables documented in its source. Those tests are skipped in normal model-free CI. Keep their logs and fixtures local.
