#!/usr/bin/env bash
# Compatibility entry point. Downloads are locked and verified before extraction.
set -euo pipefail
cd "$(dirname "$0")/.."
exec python tools/download_runtime.py --group runtime "$@"
