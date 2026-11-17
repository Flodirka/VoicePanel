"""Select installed inference backends for the next service start."""
import argparse
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "service"))
from voiceservice.config import load_config
from voiceservice import engines


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="config.yaml")
    parser.add_argument("--tts", choices=("omnivoice", "audiocpp"))
    parser.add_argument("--asr", choices=("audiocpp", "whisper"))
    args = parser.parse_args()
    cfg = load_config(args.config)
    choices = {k: v for k, v in {"tts": args.tts, "asr": args.asr}.items() if v}
    if choices:
        try:
            engines.save(cfg, choices)
        except ValueError as exc:
            parser.error(str(exc))
    print(json.dumps(engines.snapshot(cfg), ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
