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
6. Make the target unavailable and attempt insertion. Start another recording without pressing Copy. Check that capture starts, the earlier text remains in History, and restoring it while stopped keeps the original insertion progress. Restore the target and retry; only the uninserted part should be sent.
7. Test a silence reset longer than the processing time, then shorter than the processing time. Original text and translations must remain available in History. Repeat with storage disabled and with an unwritable output folder. Neither failure should lock capture or prevent completed quick documents from moving to History.
8. Check that both the toolbar and Preview are frameless.
9. Check auto-save before Stop and without insertion. Verify original text, completed results and reviewed edits in the effective transcript folder. Test enabled auto-save with no custom folder, then disable auto-save and confirm that automatic writes stop.
10. Produce several corrections. One Accept or Reject click must resolve all current corrections. Later results must preserve that decision and manual edits.

Repeat insertion checks in the applications you intend to use. Notepad coverage does not establish compatibility with every application or protected field.

The opt-in `desktopInsertionIntoFocusedTarget` test uses `VOICEPANEL_TARGET_TEST=1` and a unique local file path in `VOICEPANEL_TARGET_CONTINUE`. Focus the intended field and press Ctrl+Alt+D. Press it again when `target-insertion-stage.txt` says `stop-pending`. At `verify-target`, inspect the actual application field for both sentences in order, then create the confirmation file. The test must not be counted as a pass from its internal insertion state alone. Use a dedicated draft and do not submit messages. Keep the confirmation file and logs local.

## Real-model checks

Start the configured local service and use `tools/verify_service.py` with a synthetic or explicitly approved reference recording. Its output stays under the ignored runtime directory. Do not upload generated reports or audio by default.

The panel's opt-in inference test needs a local `live-fixture.wav` in `panel/build`. The interactive dictation test needs a dedicated Notepad document and explicit test environment variables documented in its source. Those tests are skipped in normal model-free CI. Keep their logs and fixtures local.
