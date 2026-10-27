# Third-party notices

The root MIT license applies to original VoicePanel work. It does not replace the terms of upstream adaptations, bundled fonts and icons, separately downloaded engines or model weights.

## Voice workflow and speech pipeline

VoicePanel's continuous dictation, insertion semantics and queued speech pipeline were informed by and adapted from [Pithagoras](https://github.com/thecodacus/pithagoras), including its web speech pipeline. The local reference revision was `77505ed5026117a8e3574d265d9df9a2ac6725a8`. The implementation here uses C++/Qt and Python and includes VoicePanel-specific changes for Windows capture, focus, insertion and session finalization.

```text
Pithagoras
Copyright 2026 Anirban Kar and Contributors
```

Pithagoras is licensed under Apache-2.0. Its [license](licenses/Apache-2.0.txt) and [notice](licenses/Pithagoras-NOTICE.txt) are retained. Adaptations retain the applicable upstream terms.

## Bundled assets

- **Inter.** Copyright 2020 The Inter Project Authors. The variable font in `panel/resources/fonts/` is distributed under the [SIL Open Font License 1.1](panel/resources/fonts/OFL.txt).
- **Material Line Icons.** Vjacheslav Trushkin. Static paths in the panel icon set include Material Line Icons, with animation removed and local sizing/color handling. See the [upstream project](https://github.com/cyberalien/line-md) and [MIT notice](licenses/Material-Line-Icons-MIT.txt).
- **Feather icons.** Cole Bemis. The panel's simple stroke icon set includes Feather-style icon geometry. See the [upstream project](https://github.com/feathericons/feather) and [MIT notice](licenses/Feather-MIT.txt).
- **README graphics.** The VoicePanel and profile banners and support-button graphics were created for these repositories. Screenshots show VoicePanel widgets with sample text.

## External runtime dependencies

These components are not included as binary artifacts in this source repository:

| Component | Upstream and license reference |
|---|---|
| audio.cpp | [0xShug0/audio.cpp](https://github.com/0xShug0/audio.cpp), Apache-2.0 |
| llama.cpp | [ggml-org/llama.cpp](https://github.com/ggml-org/llama.cpp), MIT |
| whisper.cpp | [ggml-org/whisper.cpp](https://github.com/ggml-org/whisper.cpp), MIT |
| Silero VAD | [snakers4/silero-vad](https://github.com/snakers4/silero-vad), MIT |
| ONNX Runtime | [microsoft/onnxruntime](https://github.com/microsoft/onnxruntime), MIT |
| Qt | [Qt licensing](https://www.qt.io/licensing/), terms depend on the modules and distribution |

Python dependency versions are declared in `service/requirements.txt`. Their upstream licenses remain applicable. Downloaded model repositories and voice datasets have separate model cards and license terms. Check those terms before using or redistributing weights, generated audio or a packaged runtime. This repository's MIT license grants no rights to a third party's voice.

`tools/downloads.lock.json` records pinned download URLs and SHA-256 values. Integrity verification confirms that the bytes match the reviewed lock; it does not certify the security of upstream software or models. Any binary distribution must preserve the relevant upstream notices and meet its dependency license obligations.
