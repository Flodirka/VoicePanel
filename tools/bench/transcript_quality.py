"""Final-transcript quality for the long meeting/caption runs, and the offline
re-pass that a saved recording can get after the call.

Three steps, one file:

  reference  Turn a word-level annotation of a looped fixture into one reference
             transcript on the fixture's own timeline.
  score      Score one transcript against that reference: WER, CER, lost
             phrases, covered speech, speaker-name accuracy, term recall.
  offline    Diarise the saved WAV in one request, re-recognise every diarised
             turn, clean the paragraphs with the text model (meeting notes and
             the context document as context), write a Markdown transcript with
             names, then score it next to the live run.

All three use the same models the service already runs; nothing is downloaded.
The offline pass reads the WAV that the panel kept, so it hears the whole
recording at once instead of one VAD phrase at a time.

Examples (from the repository root, service on 127.0.0.1:8080):

  python tools/bench/transcript_quality.py reference \
      --annotation runtime/bench/nemo-src/test_files/diar/ami_en2002d_2132.json \
      --period 61.2 --duration 600 \
      --out runtime/bench/meeting-3voices-ref.json

  python tools/bench/transcript_quality.py score \
      --report runtime/reports/subtitles-long/result.json \
      --reference runtime/bench/meeting-3voices-ref.json

  python tools/bench/transcript_quality.py offline \
      --wav runtime/bench/meeting-3voices-16k.wav \
      --reference runtime/bench/meeting-3voices-ref.json \
      --live runtime/reports/subtitles-long/result.json \
      --notes runtime/bench/meeting-notes.md \
      --names "SPEAKER_00=Лёша,SPEAKER_01=Женя,SPEAKER_02=Nik" \
      --title "Рабочий созвон" \
      --out runtime/reports/subtitles-long/offline

Reference and live reports stay under the ignored runtime directory. The
fixture is the 60 s AMI sample (ami_en2002d_2132) looped with a 1.2 s gap; the
annotation that ships with it is human ground truth, not a second ASR pass.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import math
import re
import sys
import time
import wave
from array import array
from collections import Counter, defaultdict
from pathlib import Path
from typing import Any, Iterable

import httpx

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parent))

from metrics import cer, normalize_for_wer, wer  # noqa: E402

DEFAULT_REFERENCE = ROOT / "runtime" / "bench" / "meeting-3voices-ref.json"
DEFAULT_LIVE = ROOT / "runtime" / "reports" / "subtitles-long" / "result.json"
DEFAULT_CACHE = ROOT / "runtime" / "bench" / "offline-asr-cache.json"
CONTEXT = ROOT / "panel" / "resources" / "default-context.md"

STOPWORDS = set("""
a about all also an and any are as at be because been before but by can could
did do does doing for from had has have he her here him his how i if in into is
it its just like me more my no not now of on one only or other our out over
said same she should so some than that the their them then there these they
this those to too two up us very was we were what when where which while who
why will with would you your yeah okay okay hmm um uh oh right well think know
got get going want really something thing things lot bit
""".split())


# ------------------------------------------------------------------ audio I/O
def read_wav(path: Path) -> tuple[array, int]:
    with wave.open(str(path)) as src:
        if src.getnchannels() != 1 or src.getsampwidth() != 2:
            raise SystemExit(f"{path} must be 16-bit mono, got {src.getnchannels()} ch/{src.getsampwidth() * 8} bit")
        rate = src.getframerate()
        pcm = array("h")
        pcm.frombytes(src.readframes(src.getnframes()))
    return pcm, rate


def wav_bytes(pcm: array, rate: int = 16000) -> bytes:
    out = io.BytesIO()
    with wave.open(out, "wb") as dst:
        dst.setnchannels(1)
        dst.setsampwidth(2)
        dst.setframerate(rate)
        dst.writeframes(pcm.tobytes())
    return out.getvalue()


def seconds(pcm: array, rate: int) -> float:
    return len(pcm) / float(rate)


def energy(pcm: array, start: int, length: int) -> float:
    end = min(len(pcm), start + length)
    if end <= start:
        return 0.0
    return math.sqrt(sum(v * v for v in pcm[start:end]) / float(end - start))


def quiet_cut(pcm: array, start: int, hard_end: int, rate: int) -> int:
    """Cut near hard_end at the quietest 50 ms frame, so a word is not split."""
    frame = int(0.05 * rate)
    first = max(start + frame * 8, hard_end - int(2.5 * rate))
    best, best_at = None, hard_end
    for at in range(first, hard_end - frame, frame):
        level = energy(pcm, at, frame)
        if best is None or level < best:
            best, best_at = level, at + frame // 2
    return min(hard_end, max(start + frame, best_at))


# ------------------------------------------------------------------ service
class Service:
    def __init__(self, base: str) -> None:
        self.client = httpx.Client(base_url=base.rstrip("/"), timeout=900, trust_env=False)

    def diarize(self, pcm: array, model: str = "nemotron-diar") -> list[dict[str, Any]]:
        response = self.client.post(
            "/v1/audio/diarize",
            files={"file": ("call.wav", wav_bytes(pcm), "audio/wav")},
            data={"model": model},
        )
        response.raise_for_status()
        return sorted(response.json()["speaker_turns"], key=lambda t: t["start_sample"])

    def transcribe(self, pcm: array, model: str, language: str | None, prompt: str | None) -> str:
        data: dict[str, str] = {"response_format": "json", "model": model}
        if language:
            data["language"] = language
        if prompt:
            data["prompt"] = prompt
        response = self.client.post(
            "/v1/audio/transcriptions",
            files={"file": ("turn.wav", wav_bytes(pcm), "audio/wav")},
            data=data,
        )
        response.raise_for_status()
        return str(response.json().get("text", "")).strip()

    def clean(self, text: str, mode: str, profile: str, context: str | None,
              dictionary: list[str] | None, previous: str | None) -> tuple[str, str]:
        payload: dict[str, Any] = {"text": text, "mode": mode, "profile": profile}
        if context:
            payload["context"] = context
        if dictionary:
            payload["dictionary"] = dictionary
        if previous:
            payload["previous"] = previous
        response = self.client.post("/v1/text/process", json=payload)
        response.raise_for_status()
        body = response.json()
        return str(body.get("result", text)), str(body.get("notes", ""))


# --------------------------------------------------------------- reference
def build_reference(annotation: Path, period: float, duration: float, source: str) -> dict[str, Any]:
    raw = json.loads(annotation.read_text(encoding="utf-8"))
    base: list[dict[str, Any]] = []
    for row in raw:
        words = str(row.get("words", "")).strip()
        start, end = float(row.get("start_time", 0.0)), float(row.get("end_time", 0.0))
        if not words or end <= max(0.0, start) or end <= start:
            continue
        base.append({"start": max(0.0, start), "end": max(0.0, end),
                     "speaker": str(row.get("speaker", "")), "text": words})
    segments: list[dict[str, Any]] = []
    copy = 0
    while copy * period < duration:
        for row in base:
            start, end = row["start"] + copy * period, row["end"] + copy * period
            if start >= duration:
                continue
            segments.append({"start": round(start, 3), "end": round(min(end, duration), 3),
                             "speaker": row["speaker"], "text": row["text"]})
        copy += 1
    segments.sort(key=lambda s: s["start"])
    return {"source": source, "period_s": period, "duration_s": duration, "segments": segments}


def load_reference(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


# ----------------------------------------------------------------- scoring
def words(text: str) -> list[str]:
    return normalize_for_wer(text).split()


def segment_overlap(a: dict[str, Any], b: dict[str, Any]) -> float:
    return max(0.0, min(a["end"], b["end"]) - max(a["start"], b["start"]))


def transcript_text(segments: Iterable[dict[str, Any]]) -> str:
    return " ".join(str(s.get("text", "")).strip() for s in segments if str(s.get("text", "")).strip())


def name_accuracy(hyp: list[dict[str, Any]], ref: list[dict[str, Any]]) -> dict[str, Any]:
    """Best one-to-one mapping of live labels onto reference speakers, then the
    share of hyp speech seconds that the mapped speaker explains."""
    votes: dict[str, Counter] = defaultdict(Counter)
    for seg in hyp:
        if not seg.get("speaker") or not str(seg.get("text", "")).strip():
            continue
        for other in ref:
            hit = segment_overlap(seg, other)
            if hit > 0:
                votes[seg["speaker"]][other["speaker"]] += hit
    mapping: dict[str, str] = {}
    used: set[str] = set()
    pairs = sorted(((label, speaker, count) for label, counts in votes.items()
                    for speaker, count in counts.items()), key=lambda p: -p[2])
    for label, speaker, _ in pairs:
        if label in mapping or speaker in used:
            continue
        mapping[label] = speaker
        used.add(speaker)
    right = total = 0.0
    for seg in hyp:
        if not seg.get("speaker") or not str(seg.get("text", "")).strip():
            continue
        seconds_of_speech = seg["end"] - seg["start"]
        total += seconds_of_speech
        truth = mapping.get(seg["speaker"])
        if truth is None:
            continue
        hit = sum(segment_overlap(seg, other) for other in ref if other["speaker"] == truth)
        right += min(seconds_of_speech, hit)
    return {"accuracy": round(right / total, 3) if total else None,
            "mapping": mapping, "seconds": round(total, 1)}


LOST_TURN_RECALL = 0.5


def lost_phrases(hyp: list[dict[str, Any]], ref: list[dict[str, Any]],
                 threshold: float = LOST_TURN_RECALL) -> dict[str, Any]:
    """Human turns whose words the captions covering them mostly do not carry.

    Timing alone cannot say a turn was lost: captions sit wall to wall, so every
    turn has a caption above it even when the model returned one word for it.
    A turn is lost when fewer than `threshold` of its words appear in the text
    of the captions that overlap its span; `missing_word_share` is the share of
    checked words that nothing in the captions echoes.
    """
    checked = lost = 0
    checked_words = lost_words = missing_words = 0
    for turn in ref:
        turn_words = words(turn["text"])
        if len(turn_words) < 3 or turn["end"] - turn["start"] < 0.8:
            continue
        checked += 1
        checked_words += len(turn_words)
        heard: Counter = Counter()
        for seg in hyp:
            if not str(seg.get("text", "")).strip() or segment_overlap(seg, turn) <= 0:
                continue
            heard.update(words(seg["text"]))
        kept = sum(min(count, heard[word]) for word, count in Counter(turn_words).items())
        missing_words += len(turn_words) - kept
        if kept < threshold * len(turn_words):
            lost += 1
            lost_words += len(turn_words)
    return {"checked": checked, "lost": lost,
            "share": round(lost / checked, 3) if checked else None,
            "word_share": round(lost_words / checked_words, 3) if checked_words else None,
            "missing_word_share": round(missing_words / checked_words, 3) if checked_words else None,
            "recall_threshold": threshold}


def reference_terms(ref: list[dict[str, Any]], glossary: list[str]) -> list[str]:
    """The user's glossary terms that the recording actually contains, plus the
    recording's own frequent content words (a proxy for its domain vocabulary)."""
    text = transcript_text(ref).lower()
    chosen = [t for t in glossary if t.strip() and re.search(rf"\b{re.escape(t.strip().lower())}", text)]
    counts = Counter(w for w in words(text) if len(w) >= 4 and w not in STOPWORDS)
    chosen += [w for w, n in counts.most_common(40) if n >= 3 and w not in {c.lower() for c in chosen}]
    return chosen[:60]


def term_recall(ref: list[dict[str, Any]], hyp: list[dict[str, Any]], glossary: list[str]) -> dict[str, Any]:
    terms = reference_terms(ref, glossary)
    said = transcript_text(hyp).lower()
    missing = [t for t in terms if not re.search(rf"\b{re.escape(t.lower())}", said)]
    return {"terms": len(terms), "kept": len(terms) - len(missing),
            "accuracy": round((len(terms) - len(missing)) / len(terms), 3) if terms else None,
            "missing": missing[:15]}


def empty_captions(hyp: list[dict[str, Any]]) -> dict[str, Any]:
    """Segments the recogniser was given but returned no words for: a phrase
    the live run never showed. Different from an uncovered human turn: the
    audio reached the model and the model said nothing."""
    empty = sum(1 for s in hyp if not str(s.get("text", "")).strip())
    return {"segments": len(hyp), "empty": empty,
            "share": round(empty / len(hyp), 3) if hyp else None}


def score(hyp: list[dict[str, Any]], ref: list[dict[str, Any]], glossary: list[str]) -> dict[str, Any]:
    hyp_text, ref_text = transcript_text(hyp), transcript_text(ref)
    ref_words, hyp_words = words(ref_text), words(hyp_text)
    ref_seconds = sum(s["end"] - s["start"] for s in ref)
    covered = 0.0
    for seg in hyp:
        if not str(seg.get("text", "")).strip():
            continue
        for other in ref:
            covered += segment_overlap(seg, other)
    covered = min(covered, ref_seconds)
    return {
        "words": {"reference": len(ref_words), "transcript": len(hyp_words),
                  "ratio": round(len(hyp_words) / len(ref_words), 3) if ref_words else None},
        "wer": round(wer(ref_text, hyp_text), 4),
        "cer": round(cer(ref_text, hyp_text), 4),
        "speech_seconds": {"reference": round(ref_seconds, 1), "captioned": round(covered, 1),
                           "share": round(covered / ref_seconds, 3) if ref_seconds else None},
        "lost_phrases": lost_phrases(hyp, ref),
        "empty_captions": empty_captions(hyp),
        "speaker_names": name_accuracy(hyp, ref),
        "terms": term_recall(ref, hyp, glossary),
    }


def live_segments(report: dict[str, Any]) -> list[dict[str, Any]]:
    """Per-phrase rows written by the caption acceptance test."""
    out: list[dict[str, Any]] = []
    for row in report.get("phrases", []):
        start = row.get("start_s", row.get("at_s"))
        end = row.get("end_s")
        if start is None or end is None:
            continue
        out.append({"start": float(start), "end": float(end),
                    "speaker": str(row.get("speaker", "")), "text": str(row.get("text", ""))})
    return out


def glossary_from_context(path: Path) -> list[str]:
    """The panel's own terminology section: bullets under a Terminology heading."""
    if not path.is_file():
        return []
    heading = re.compile(r"^\s*#+\s*.*(terminolog|терминолог|glossary|словарь)", re.IGNORECASE)
    any_heading = re.compile(r"^\s*#")
    bullet = re.compile(r"^\s*[-*•]\s+")
    terms: list[str] = []
    inside = False
    for line in path.read_text(encoding="utf-8").splitlines():
        if any_heading.match(line):
            inside = bool(heading.match(line))
            continue
        match = bullet.match(line)
        if inside and match:
            term = line[match.end():].strip()
            if term and term.lower() not in {t.lower() for t in terms}:
                terms.append(term)
    return terms


# ------------------------------------------------------------------ offline
def merge_turns(turns: list[dict[str, Any]], rate: int, min_s: float = 0.4, gap_s: float = 0.5) -> list[dict[str, Any]]:
    merged: list[dict[str, Any]] = []
    for turn in turns:
        start, end = turn["start_sample"] / rate, turn["end_sample"] / rate
        speaker = str(turn["speaker_id"])
        if end - start < min_s:
            continue
        if merged and merged[-1]["speaker"] == speaker and start - merged[-1]["end"] <= gap_s:
            merged[-1]["end"] = end
        else:
            merged.append({"start": start, "end": end, "speaker": speaker})
    return merged


def block_windows(pcm: array, rate: int, silence_s: float = 0.6, min_s: float = 1.0,
                  limit_s: float = 12.0) -> list[tuple[int, int]]:
    """Contiguous ASR windows for the offline rebuild: a pause ends a window, a
    hard cap splits a long stretch. A real meeting has few clean 0.6 s pauses,
    so this is mostly wall-to-wall 12 s blocks, which keeps the recogniser's
    context and leaves no speech between requests. Speakers are then assigned
    from the whole-file diarisation."""
    frame = int(0.05 * rate)
    pause = int(silence_s * rate)
    pieces: list[tuple[int, int]] = []
    start, silence = -1, 0
    for offset in range(0, len(pcm) - frame, frame):
        block = pcm[offset:offset + frame]
        speech = math.sqrt(sum(v * v for v in block) / len(block)) > 0.01
        if speech:
            if start < 0:
                start = offset
            silence = 0
        elif start >= 0:
            silence += frame
            if silence >= pause:
                if offset + frame - start >= min_s * rate:
                    pieces.append((start, offset + frame))
                start, silence = -1, 0
        if start >= 0 and offset - start >= limit_s * rate:
            pieces.append((start, start + int(limit_s * rate)))
            start, silence = -1, 0
    if start >= 0 and len(pcm) - start >= min_s * rate:
        pieces.append((start, len(pcm)))
    return pieces


def assign_speakers(pieces: list[tuple[int, int]], turns: list[dict[str, Any]], rate: int) -> list[dict[str, Any]]:
    """Each VAD phrase gets the voice that holds most of its audio. A phrase
    that no turn covers (breath, noise) goes to the nearest turn in time."""
    out: list[dict[str, Any]] = []
    for begin, end in pieces:
        start, stop = begin / rate, end / rate
        votes: Counter = Counter()
        for turn in turns:
            hit = min(stop, turn["end_sample"] / rate) - max(start, turn["start_sample"] / rate)
            if hit > 0:
                votes[str(turn["speaker_id"])] += hit
        speaker = votes.most_common(1)[0][0] if votes else None
        if speaker is None:
            nearest = min(turns, key=lambda t: min(abs(start - t["start_sample"] / rate),
                                                  abs(start - t["end_sample"] / rate)), default=None)
            speaker = str(nearest["speaker_id"]) if nearest else "speaker_?"
        out.append({"start": round(start, 2), "end": round(stop, 2),
                    "speaker": speaker, "text": ""})
    return out


def split_turn(pcm: array, rate: int, start: float, end: float, limit_s: float) -> list[tuple[int, int]]:
    """Turn bounds in samples, cut at quiet points into <= limit_s pieces."""
    begin, finish = int(start * rate), min(len(pcm), int(end * rate))
    pieces: list[tuple[int, int]] = []
    while finish - begin > int(limit_s * rate):
        cut = quiet_cut(pcm, begin, begin + int(limit_s * rate), rate)
        pieces.append((begin, cut))
        begin = cut
    if finish - begin > 0:
        pieces.append((begin, finish))
    return pieces


class AsrCache:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data: dict[str, str] = {}
        if path.is_file():
            try:
                self.data = json.loads(path.read_text(encoding="utf-8"))
            except ValueError:
                self.data = {}
        self.dirty = False

    @staticmethod
    def key(pcm: array, model: str, language: str | None) -> str:
        digest = hashlib.sha1(pcm.tobytes()).hexdigest()
        return f"{model}|{language or 'auto'}|{digest}"

    def get(self, key: str) -> str | None:
        return self.data.get(key)

    def put(self, key: str, text: str) -> None:
        self.data[key] = text
        self.dirty = True

    def save(self) -> None:
        if self.dirty:
            self.path.parent.mkdir(parents=True, exist_ok=True)
            self.path.write_text(json.dumps(self.data, ensure_ascii=False), encoding="utf-8")


def transcribe_windows(service: Service, cache: AsrCache, pcm: array, windows: list[tuple[int, int]],
                       segments: list[dict[str, Any]], args) -> None:
    """Fill each segment's text from the recogniser, one request per window."""
    for (begin, end), segment in zip(windows, segments):
        chunk = pcm[begin:end]
        key = AsrCache.key(chunk, args.asr_model, args.language)
        text = cache.get(key)
        if text is None:
            text = service.transcribe(chunk, args.asr_model, args.language, args.asr_prompt)
            cache.put(key, text)
        segment["text"] = text


def offline_pass(args, service: Service, pcm: array, rate: int) -> tuple[dict[str, list[dict[str, Any]]], list[dict[str, Any]], dict[str, Any]]:
    """Two offline readings of the same saved audio:

    turn   one diarised turn per request: the literal "re-recognise every
           speaker turn" pass. Short turns cost the recogniser its context.
    block  contiguous 12 s ASR windows, each named by the whole-file
           diarisation: no VAD gaps, more words, coarser names when two people
           speak inside one block.
    """
    started = time.perf_counter()
    turns = service.diarize(pcm, args.diar_model)
    diarized_s = time.perf_counter() - started
    merged = merge_turns(turns, rate)

    cache = AsrCache(Path(args.cache))
    asr_started = time.perf_counter()

    turn_windows: list[tuple[int, int]] = []
    turn_segments: list[dict[str, Any]] = []
    counts: list[int] = []
    for turn in merged:
        pieces = split_turn(pcm, rate, turn["start"], turn["end"], args.chunk_s)
        turn_windows += pieces
        counts.append(len(pieces))
        turn_segments.append({"start": round(turn["start"], 2), "end": round(turn["end"], 2),
                              "speaker": turn["speaker"], "text": ""})
    # A turn split into several requests still yields one text: transcribe the
    # pieces, then join them onto the turn they came from.
    piece_segments = [{"start": 0.0, "end": 0.0, "speaker": "", "text": ""} for _ in turn_windows]
    transcribe_windows(service, cache, pcm, turn_windows, piece_segments, args)
    piece_at = 0
    for segment, count in zip(turn_segments, counts):
        segment["text"] = " ".join(piece_segments[piece_at + i]["text"] for i in range(count)).strip()
        piece_at += count

    block_windows_list = block_windows(pcm, rate)
    block_segments = assign_speakers(block_windows_list, turns, rate)
    transcribe_windows(service, cache, pcm, block_windows_list, block_segments, args)
    asr_s = time.perf_counter() - asr_started
    cache.save()

    readings = {"turns": turn_segments, "blocks": block_segments}
    paragraphs = paragraphise(readings[args.source], args.paragraph_chars)
    cleanup: dict[str, Any] = {"paragraphs": 0, "changed": 0, "skipped": 0, "failed": 0,
                              "seconds": 0.0}
    if args.clean:
        cleanup = clean_paragraphs(service, paragraphs, args)
    return (readings, paragraphs, {
        "diarize_s": round(diarized_s, 1),
        "asr_s": round(asr_s, 1),
        "clean_s": cleanup["seconds"],
        "cleanup": cleanup,
        "audio_s": round(seconds(pcm, rate), 1),
        "rtf": round((diarized_s + asr_s + cleanup["seconds"]) / max(1e-9, seconds(pcm, rate)), 3),
        "turns": len(turn_segments),
        "blocks": len(block_segments),
        "diarizer_speakers": len({t["speaker"] for t in turn_segments}),
    })


def paragraphise(segments: list[dict[str, Any]], max_chars: int) -> list[dict[str, Any]]:
    """Consecutive turns from one speaker become one paragraph, split on length."""
    paragraphs: list[dict[str, Any]] = []
    for seg in segments:
        if not seg["text"]:
            continue
        if paragraphs and paragraphs[-1]["speaker"] == seg["speaker"] \
                and len(paragraphs[-1]["text"]) + len(seg["text"]) < max_chars:
            paragraphs[-1]["text"] += " " + seg["text"]
            paragraphs[-1]["end"] = seg["end"]
        else:
            paragraphs.append({"speaker": seg["speaker"], "start": seg["start"],
                               "end": seg["end"], "text": seg["text"]})
    return paragraphs


def clean_paragraphs(service: Service, paragraphs: list[dict[str, Any]], args) -> dict[str, Any]:
    """Clean each speaker paragraph in place; the ASR text stays in `raw`.
    A service with llm.skip_if_clean (the default) answers "clean" without
    calling the model, which is reported instead of passing for a cleanup."""
    notes = args.notes_text.strip()
    context = ""
    if args.context_file and Path(args.context_file).is_file():
        context = Path(args.context_file).read_text(encoding="utf-8").strip()
    if notes:
        context = (context + "\n\n" if context else "") + (
            "Meeting notes. Use them only to resolve names, terms and what the speakers refer to; "
            "never add a fact that is not in the transcript:\n" + notes)
    if args.task:
        context = (context + "\n\n" if context else "") + f"Meeting task: {args.task}"
    glossary = args.glossary or glossary_from_context(CONTEXT)
    started = time.perf_counter()
    previous = None
    changed = skipped = failed = 0
    for paragraph in paragraphs:
        try:
            result, note = service.clean(paragraph["text"], args.mode, args.profile, context,
                                         glossary, previous)
        except Exception as exc:                      # a failed paragraph keeps its ASR text
            paragraph["error"] = str(exc)[:200]
            failed += 1
            previous = paragraph["text"][-600:]
            continue
        if result.strip() and result.strip() != paragraph["text"].strip():
            paragraph["raw"] = paragraph["text"]
            paragraph["text"] = result.strip()
            changed += 1
            if note:
                paragraph["note"] = note
        elif note == "clean":
            skipped += 1
        previous = paragraph["text"][-600:]
    return {"seconds": round(time.perf_counter() - started, 1), "paragraphs": len(paragraphs),
            "changed": changed, "skipped": skipped, "failed": failed}


def write_markdown(path: Path, args, paragraphs: list[dict[str, Any]], quality: dict[str, Any],
                   meta: dict[str, Any]) -> None:
    names = parse_names(args.names)
    stamp = time.strftime("%Y-%m-%d %H:%M")
    lines = [f"# {args.title or 'Meeting'} — {stamp}", ""]
    notes = args.notes_text.strip()
    if notes:
        lines += ["## Notes", notes, ""]
    lines += ["## Transcript", ""]
    previous = None
    for paragraph in paragraphs:
        if not paragraph["text"]:
            continue
        who = names.get(paragraph["speaker"], paragraph["speaker"])
        if who != previous:
            lines.append(f"**{who}**")
            previous = who
        lines.append(f"- `{clock(paragraph['start'])}` {paragraph['text']}")
    if quality:
        lines += ["", "## Quality", "", "| metric | value |", "| --- | --- |"]
        for key, value in quality.items():
            lines.append(f"| {key} | {value} |")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def clock(value: float) -> str:
    total = int(round(value))
    return f"{total // 3600:02d}:{total % 3600 // 60:02d}:{total % 60:02d}"


def parse_names(text: str | None) -> dict[str, str]:
    out: dict[str, str] = {}
    if not text:
        return out
    for item in text.split(","):
        key, _, value = item.partition("=")
        if key.strip() and value.strip():
            out[key.strip()] = value.strip()
    return out


# -------------------------------------------------------------------- CLI
def command_reference(args) -> int:
    annotation = Path(args.annotation)
    reference = build_reference(annotation, args.period, args.duration, annotation.name)
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(reference, indent=1, ensure_ascii=False), encoding="utf-8")
    speakers = sorted({s["speaker"] for s in reference["segments"]})
    print(f"{out}: {len(reference['segments'])} turns, {len(words(transcript_text(reference['segments'])))} words, "
          f"speakers {speakers}")
    return 0


def command_score(args) -> int:
    reference = load_reference(Path(args.reference))
    report = json.loads(Path(args.report).read_text(encoding="utf-8"))
    hyp = live_segments(report)
    if not hyp:
        raise SystemExit(f"{args.report} has no per-phrase timings; rebuild the panel tests and rerun the acceptance")
    glossary = args.glossary or glossary_from_context(Path(args.context_file))
    result = score(hyp, reference["segments"], glossary)
    result["hypothesis"] = {"source": str(args.report), "segments": len(hyp),
                            "empty": sum(1 for s in hyp if not s["text"].strip())}
    out = Path(args.out or Path(args.report).with_name("quality.json"))
    out.write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding="utf-8")
    print_table([("WER", result["wer"]), ("CER", result["cer"]),
                 ("words kept", result["words"]["ratio"]),
                 ("speech covered", result["speech_seconds"]["share"]),
                 ("lost phrases", result["lost_phrases"]["share"]),
                 ("lost words", result["lost_phrases"]["word_share"]),
                 ("speaker names", result["speaker_names"]["accuracy"]),
                 ("terms", result["terms"]["accuracy"])])
    print(f"full results: {out}")
    return 0


def command_offline(args) -> int:
    wav = Path(args.wav)
    pcm, rate = read_wav(wav)
    service = Service(args.base)
    print(f"offline: {seconds(pcm, rate):.1f} s of audio from {wav.name}")
    readings, paragraphs, meta = offline_pass(args, service, pcm, rate)
    print(f"offline: {meta['turns']} diarised turns, {meta['blocks']} ASR blocks, "
          f"{meta['diarizer_speakers']} voices, transcript from {args.source}, "
          f"diarise {meta['diarize_s']} s, asr {meta['asr_s']} s, clean {meta['clean_s']} s, "
          f"RTF {meta['rtf']}")
    cleanup = meta.get("cleanup") or {}
    if cleanup.get("skipped"):
        print(f"note: the service skipped the text model on {cleanup['skipped']} of "
              f"{cleanup['paragraphs']} paragraphs (llm.skip_if_clean is on); "
              f"run with a config that sets it to false to measure cleanup")
    elif args.clean:
        print(f"cleanup: {cleanup.get('changed')} of {cleanup.get('paragraphs')} paragraphs rewritten"
              + (f", {cleanup['failed']} failed" if cleanup.get("failed") else ""))

    reference = load_reference(Path(args.reference))
    glossary = args.glossary or glossary_from_context(CONTEXT)
    # The final transcript is the cleaned, paragraph-level one; the raw readings
    # stay next to it so cleanup can be judged instead of assumed.
    final = [{"start": p["start"], "end": p["end"], "speaker": p["speaker"], "text": p["text"]}
             for p in paragraphs]
    offline_quality = score(final, reference["segments"], glossary)
    turn_quality = score(readings["turns"], reference["segments"], glossary)
    block_quality = score(readings["blocks"], reference["segments"], glossary)
    comparison: dict[str, Any] = {"offline": offline_quality, "offline_raw": block_quality,
                                  "offline_turns": turn_quality}
    table = [("offline WER", offline_quality["wer"]), ("offline CER", offline_quality["cer"]),
             ("offline WER before cleanup", block_quality["wer"]),
             ("offline lost phrases", offline_quality["lost_phrases"]["share"]),
             ("offline names", offline_quality["speaker_names"]["accuracy"]),
             ("offline terms", offline_quality["terms"]["accuracy"])]
    if args.live and Path(args.live).is_file():
        live = live_segments(json.loads(Path(args.live).read_text(encoding="utf-8")))
        live_quality = score(live, reference["segments"], glossary)
        comparison["live"] = live_quality
        table = [("metric", "live", "offline turns", "offline blocks", "offline final")] + [
            ("WER", live_quality["wer"], turn_quality["wer"], block_quality["wer"], offline_quality["wer"]),
            ("CER", live_quality["cer"], turn_quality["cer"], block_quality["cer"], offline_quality["cer"]),
            ("words kept", live_quality["words"]["ratio"], turn_quality["words"]["ratio"],
             block_quality["words"]["ratio"], offline_quality["words"]["ratio"]),
            ("speech covered", live_quality["speech_seconds"]["share"], turn_quality["speech_seconds"]["share"],
             block_quality["speech_seconds"]["share"], offline_quality["speech_seconds"]["share"]),
            ("lost phrases", live_quality["lost_phrases"]["share"], turn_quality["lost_phrases"]["share"],
             block_quality["lost_phrases"]["share"], offline_quality["lost_phrases"]["share"]),
            ("empty captions", live_quality["empty_captions"]["share"], turn_quality["empty_captions"]["share"],
             block_quality["empty_captions"]["share"], offline_quality["empty_captions"]["share"]),
            ("names", live_quality["speaker_names"]["accuracy"], turn_quality["speaker_names"]["accuracy"],
             block_quality["speaker_names"]["accuracy"], offline_quality["speaker_names"]["accuracy"]),
            ("terms", live_quality["terms"]["accuracy"], turn_quality["terms"]["accuracy"],
             block_quality["terms"]["accuracy"], offline_quality["terms"]["accuracy"]),
        ]
    comparison["meta"] = meta
    comparison["names"] = parse_names(args.names)
    comparison["paragraphs"] = len(paragraphs)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "offline.json").write_text(json.dumps(comparison, indent=2, ensure_ascii=False), encoding="utf-8")
    quality = {"WER vs human": offline_quality["wer"], "CER vs human": offline_quality["cer"],
               "lost phrases": offline_quality["lost_phrases"]["share"],
               "empty captions": offline_quality["empty_captions"]["share"],
               "speaker names": offline_quality["speaker_names"]["accuracy"],
               "term recall": offline_quality["terms"]["accuracy"]}
    write_markdown(out_dir / "final.md", args, paragraphs, quality, meta)
    print_table(table)
    print(f"transcript: {out_dir / 'final.md'}\nnumbers: {out_dir / 'offline.json'}")
    return 0


def print_table(rows: list[tuple]) -> None:
    for row in rows:
        print("  " + "  ".join(str(v) for v in row))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)

    ref = sub.add_parser("reference", help="build a looped fixture's reference transcript")
    ref.add_argument("--annotation", required=True, help="word-level JSON with speaker/start_time/end_time/words")
    ref.add_argument("--period", type=float, required=True, help="seconds between repeats of the source clip")
    ref.add_argument("--duration", type=float, required=True, help="fixture duration in seconds")
    ref.add_argument("--out", default=str(DEFAULT_REFERENCE))
    ref.set_defaults(func=command_reference)

    scr = sub.add_parser("score", help="score a live caption report")
    scr.add_argument("--report", default=str(DEFAULT_LIVE))
    scr.add_argument("--reference", default=str(DEFAULT_REFERENCE))
    scr.add_argument("--context-file", default=str(CONTEXT), help="panel context document (terminology section)")
    scr.add_argument("--glossary", action="append", default=[], help="extra term (repeatable)")
    scr.add_argument("--out", default=None)
    scr.set_defaults(func=command_score)

    off = sub.add_parser("offline", help="diarise and re-recognise a saved recording, clean it, score it")
    off.add_argument("--base", default="http://127.0.0.1:8080")
    off.add_argument("--wav", required=True)
    off.add_argument("--reference", default=str(DEFAULT_REFERENCE))
    off.add_argument("--live", default=str(DEFAULT_LIVE), help="live acceptance report to compare against")
    off.add_argument("--out", default=str(ROOT / "runtime" / "reports" / "subtitles-long"))
    off.add_argument("--diar-model", default="nemotron-diar")
    off.add_argument("--asr-model", default="nemotron")
    off.add_argument("--language", default="en", help="ASR language, or 'auto'")
    off.add_argument("--asr-prompt", default=None)
    off.add_argument("--chunk-s", type=float, default=12.0, help="longest ASR chunk; cut at a quiet point")
    off.add_argument("--cache", default=str(DEFAULT_CACHE))
    off.add_argument("--clean", action=argparse.BooleanOptionalAction, default=True)
    off.add_argument("--source", choices=["turns", "blocks"], default="turns",
                     help="which reading becomes the document: per diarised turn (names) or\n"
                          "contiguous 12 s blocks (words, coarser names)")
    off.add_argument("--mode", default="correct_en")
    off.add_argument("--profile", default="general")
    off.add_argument("--context-file", default=str(CONTEXT))
    off.add_argument("--glossary", action="append", default=[])
    off.add_argument("--notes", default=None, help="file with the meeting notes used as context")
    off.add_argument("--task", default=None, help="what the meeting was for")
    off.add_argument("--title", default="Meeting")
    off.add_argument("--names", default=None, help='speaker labels: "SPEAKER_00=Лёша,SPEAKER_01=Женя"')
    off.add_argument("--paragraph-chars", type=int, default=900)
    off.set_defaults(func=command_offline)

    args = parser.parse_args()
    if getattr(args, "language", None) == "auto":
        args.language = None
    if getattr(args, "notes", None):
        args.notes_text = Path(args.notes).read_text(encoding="utf-8")
    else:
        args.notes_text = ""
    try:
        sys.stdout.reconfigure(encoding="utf-8")
    except Exception:
        pass
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
