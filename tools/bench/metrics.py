"""Text metrics for the VoicePanel benchmark: WER, CER, chrF, edit ratios and
protected-token checks. Pure Python, no third-party dependencies."""
from __future__ import annotations

import re
import statistics
import unicodedata
from collections import Counter
from typing import Iterable, Sequence

_PUNCT = re.compile(r"[^\w\s'+#./\\-]", re.UNICODE)
_SPACE = re.compile(r"\s+")


def normalize_for_wer(text: str) -> str:
    """Lowercase, NFC, ё->е, drop sentence punctuation, keep code-ish symbols."""
    text = unicodedata.normalize("NFC", text).lower().replace("ё", "е")
    text = _PUNCT.sub(" ", text)
    # trailing sentence dots/commas attached to words ("build." -> "build")
    text = re.sub(r"(?<=\w)[.,](?=\s|$)", " ", text)
    return _SPACE.sub(" ", text).strip()


def _levenshtein(a: Sequence, b: Sequence) -> int:
    if len(a) < len(b):
        a, b = b, a
    previous = list(range(len(b) + 1))
    for i, x in enumerate(a, 1):
        current = [i]
        for j, y in enumerate(b, 1):
            current.append(min(previous[j] + 1, current[j - 1] + 1, previous[j - 1] + (x != y)))
        previous = current
    return previous[-1]


def wer(reference: str, hypothesis: str) -> float:
    ref = normalize_for_wer(reference).split()
    hyp = normalize_for_wer(hypothesis).split()
    if not ref:
        return 0.0 if not hyp else 1.0
    return _levenshtein(ref, hyp) / len(ref)


def cer(reference: str, hypothesis: str) -> float:
    ref = normalize_for_wer(reference).replace(" ", "")
    hyp = normalize_for_wer(hypothesis).replace(" ", "")
    if not ref:
        return 0.0 if not hyp else 1.0
    return _levenshtein(ref, hyp) / len(ref)


def edit_ratio(a: str, b: str) -> float:
    """Character edit distance between two texts, relative to the first."""
    a, b = a.strip(), b.strip()
    if not a:
        return 0.0 if not b else 1.0
    return _levenshtein(a, b) / len(a)


def chrf(reference: str, hypothesis: str, n: int = 6, beta: float = 2.0) -> float:
    """chrF (Popovic 2015), character n-grams up to n, whitespace removed. 0..100."""
    ref = reference.replace(" ", "")
    hyp = hypothesis.replace(" ", "")
    precisions, recalls = [], []
    for k in range(1, n + 1):
        r = Counter(ref[i:i + k] for i in range(len(ref) - k + 1))
        h = Counter(hyp[i:i + k] for i in range(len(hyp) - k + 1))
        if not r or not h:
            continue
        overlap = sum((r & h).values())
        precisions.append(overlap / sum(h.values()))
        recalls.append(overlap / sum(r.values()))
    if not precisions:
        return 0.0
    p, r = statistics.mean(precisions), statistics.mean(recalls)
    if p == 0 and r == 0:
        return 0.0
    return 100.0 * (1 + beta ** 2) * p * r / (beta ** 2 * p + r)


def preserved(tokens: Iterable[str], output: str) -> tuple[int, int, list[str]]:
    """How many protected tokens appear verbatim in the output."""
    tokens = list(tokens)
    missing = [t for t in tokens if t not in output]
    return len(tokens) - len(missing), len(tokens), missing


def script_share(text: str, script: str) -> float:
    """Share of letters in Cyrillic ('ru') or Latin ('en') script."""
    letters = [c for c in text if c.isalpha()]
    if not letters:
        return 0.0
    if script == "ru":
        hits = sum(1 for c in letters if "Ѐ" <= c <= "ӿ")
    else:
        hits = sum(1 for c in letters if c.isascii())
    return hits / len(letters)


def percentile(values: Sequence[float], q: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    k = (len(ordered) - 1) * q
    lo, hi = int(k), min(int(k) + 1, len(ordered) - 1)
    return ordered[lo] + (ordered[hi] - ordered[lo]) * (k - lo)


def summary(values: Sequence[float]) -> dict[str, float | int | None]:
    return {
        "n": len(values),
        "p50": percentile(values, 0.5),
        "p95": percentile(values, 0.95),
        "mean": statistics.mean(values) if values else None,
        "max": max(values) if values else None,
    }
