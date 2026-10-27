#!/usr/bin/env bash
# Downloads Silero VAD and ONNX Runtime only. Python dependencies are installed separately.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python tools/download_runtime.py --group extras "$@"
