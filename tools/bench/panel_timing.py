"""User-facing dictation latency from the panel's timing log.

Start the panel with VOICEPANEL_TIMING_LOG set to a file path, dictate with
the protocol below, then run:

    python tools/bench/panel_timing.py <log.jsonl> [--json out.json]

Protocol: one complete sentence per utterance (ending with . ! or ?), then a
pause of at least two seconds, in Quick Dictation with a focused target such
as Notepad. Each final transcript then becomes one text request and one
insertion. Utterances whose finals were batched into one request are counted
as "ambiguous" and left out instead of being guessed.

Clock: one monotonic clock inside the panel. "speech end" is when the GUI
thread processed the last VAD frame above the speech threshold; the acoustic
end is earlier by the capture block (64 ms), the device period and VAD lag.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
from metrics import summary  # noqa: E402

STAGES = ("vad_wait_ms", "speech_end_to_final_ms", "final_to_text_ms", "text_to_insert_ms",
          "speech_end_to_text_ms", "speech_end_to_insert_ms")


def load(path: Path) -> list[dict[str, Any]]:
    events = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.strip():
            events.append(json.loads(line))
    return sorted(events, key=lambda e: e["t_ms"])


def _first(events: list[dict[str, Any]], names: tuple[str, ...], after: float) -> dict[str, Any] | None:
    return next((e for e in events if e["event"] in names and e["t_ms"] >= after), None)


def sessions(events: list[dict[str, Any]]) -> list[list[dict[str, Any]]]:
    """Segment ids restart with every dictation session: split on session_start."""
    out: list[list[dict[str, Any]]] = [[]]
    for event in events:
        if event["event"] == "session_start":
            out.append([])
        else:
            out[-1].append(event)
    return [s for s in out if s]


def analyze(events: list[dict[str, Any]]) -> dict[str, Any]:
    parts = [_analyze_session(s) for s in sessions(events)]
    rows = [r for p in parts for r in p["rows"]]
    return {
        "measurement": "panel",
        "sessions": len(parts),
        "utterances": len(rows),
        **{k: sum(p[k] for p in parts) for k in ("ambiguous", "text_failures", "not_inserted")},
        **{stage: summary([r[stage] for r in rows if stage in r]) for stage in STAGES},
        "rows": rows,
    }


def _analyze_session(events: list[dict[str, Any]]) -> dict[str, Any]:
    ends = {e["segment"]: e for e in events if e["event"] == "segment_end"}
    finals = [e for e in events if e["event"] == "final" and e.get("chars", 0) > 0 and e["segment"] in ends]
    rows, ambiguous, failed, not_inserted = [], 0, 0, 0
    for index, final in enumerate(finals):
        result = _first(events, ("text_result", "text_failed"), final["t_ms"])
        if result is None:
            continue
        nxt = finals[index + 1] if index + 1 < len(finals) else None
        if nxt is not None and nxt["t_ms"] <= result["t_ms"]:
            ambiguous += 1           # the next final arrived first: batched or queued
            continue
        if result["event"] == "text_failed":
            failed += 1
            continue
        end = ends[final["segment"]]
        speech_end = end["last_speech_ms"]
        row = {
            "segment": final["segment"],
            "vad_wait_ms": end["t_ms"] - speech_end,
            "speech_end_to_final_ms": final["t_ms"] - speech_end,
            "final_to_text_ms": result["t_ms"] - final["t_ms"],
            "speech_end_to_text_ms": result["t_ms"] - speech_end,
        }
        inserted = _first(events, ("inserted",), result["t_ms"])
        if inserted is not None and (nxt is None or inserted["t_ms"] < nxt["t_ms"]):
            row["text_to_insert_ms"] = inserted["t_ms"] - result["t_ms"]
            row["speech_end_to_insert_ms"] = inserted["t_ms"] - speech_end
            row["clipboard"] = bool(inserted.get("clipboard"))
        else:
            not_inserted += 1
        rows.append(row)
    return {"rows": rows, "ambiguous": ambiguous, "text_failures": failed, "not_inserted": not_inserted}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", type=Path)
    parser.add_argument("--json", type=Path, default=None, help="write the full result here")
    args = parser.parse_args()
    result = analyze(load(args.log))
    if args.json:
        args.json.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k != "rows"}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
