# Contributing

VoicePanel contains a Windows desktop panel and a local Python speech service. Changes should keep that boundary clear.

## Before making a change

For a bug, include the expected behavior, actual behavior, Windows version, affected application and relevant model or engine. For a feature, open an issue to describe the use case before writing a large change. Do not attach private transcripts, voice samples, tokens or machine-specific configuration.

## Development

Work, build and commit in the existing project checkout. Keep publication archives outside the source tree and do not create a second working repository under `runtime/`. Local verification notes, recordings and recovery archives stay ignored.

Follow [Getting started](docs/getting-started.md) for runtime dependencies and [Architecture](docs/architecture.md) for component ownership. Python tests can run without downloading speech models:

```powershell
python -m pip install --only-binary=:all: --require-hashes -r service/requirements.lock
python -m pip install pytest==9.1.1
python -m pytest tests -q
```

For panel changes, build with `tools/build_panel.ps1` and run CTest from a developer shell. A passing headless test does not establish microphone or external-field behavior. Test changes to hotkeys and insertion in a real Windows text field and state what was actually run.

## Pull requests

Keep the change focused. Explain the trigger, resulting behavior, checks run and remaining limitations. Preserve sentence order, manual edits and retryable text. Reuse the existing Insert path. Do not add models, runtime binaries, recordings, local state, build output or agent workspace notes. Update relevant documentation when behavior changes.

By contributing, you agree to license your original contribution under this project's MIT license. Preserve third-party notices for material you reuse.
