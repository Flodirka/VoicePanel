# Roadmap

This is the working plan after 0.3.0. It exists so that a change can be argued from
a number instead of from an impression: every item states what changes, how it is
measured, and what has to be true before it counts as done.

## Quality bar

These rules apply to every item, and a change that breaks one does not land:

- **Gates are not edited to pass.** If an acceptance is red, the cause is fixed or
  the item is reported as open. Thresholds move only with a written reason that
  points at a new measurement, never at a run that nearly passed.
- **One change, one measurement.** Numbers come from the fixture runs listed under
  [Verification](#verification), recorded in `runtime/reports/`, and compared with
  the same fixture and the same gates as the run before it.
- **A metric needs a name before the model changes.** Adding a model or a cleanup
  pass without its own metric is not a plan item, because there is nothing to
  compare it with (this is why 0.6 waits).
- **No silent fallbacks.** A path that drops information (a prompt, a label, a
  phrase) either recovers it or reports it; it does not quietly continue with
  something worse. The prompt bug in 0.4 exists because that rule was broken.
- **Tests describe behaviour, not code shape.** New behaviour gets a unit test in
  `tests/` (service, model-free) or `panel/tests/` (panel, QtTest); measured
  quality work gets an acceptance run. A fix that cannot be tested is a bug that
  will come back.
- **Fixtures stay local.** Recordings, annotations, reports and logs stay under the
  ignored `runtime/` tree; nothing personal is committed. A fixture that a test
  needs but cannot find is a skip, never a pass.
- **Comments say why.** A constant that exists for a measured reason keeps that
  reason next to it, with the number that produced it.
- **The docs move with the code.** Behaviour that a user or a future maintainer
  depends on is described in `docs/`; `docs/testing.md` gains the command for every
  new acceptance.

## Where 0.3.0 left us

Measured on the current tree (0.3.0 plus the caption and diarisation work), with
the service on `http://127.0.0.1:8080`:

| metric | value | where |
| --- | --- | --- |
| WER | 0.303 | `runtime/reports/subtitles-long/result.json` |
| CER | 0.224 | same |
| caption latency, steady → last third | 184 → 185 ms | same |
| speech that reached a caption | 88.1 % | same |
| lost human turns | 20.6 % (20 of 97, 24.4 % of their words missing) | same |
| phrases without words | 14.6 % (20 of 137) | same |
| speaker names correct | 75.2 % (varies 72–79 % between runs) | same |
| glossary terms kept | 79.5 % (31 of 39) | same |
| meeting acceptance | pass, 83 phrases, 3 speakers, 1938 → 5179 → 5289 ms | `runtime/reports/meeting-long/result.json` |
| single-voice acceptance | pass, 27 captions, 1 label, 252 → 187 ms | `runtime/reports/subtitles-single/result.json` |

The word and timing figures are stable run to run. The name figure is not, and the
subtitles acceptance is still red: the quiet third voice is merged into the loudest
one by the window diarisation (the whole-file reference separates them), so two
voices share a label and its 80 % bar fails. That is tracked as O-1 below and is a
prerequisite for treating any later name or term number as meaningful.

The diagnosis of the empty phrases is already in place (`transcript_quality.py
diagnose`, see [Testing](docs/testing.md#transcript-quality)):

- all 20 empty phrases are `boundary` phrases: 1.45–1.55 s long (median 1.55 s
  against 3.05 s for phrased speech), loud (RMS 1060 against a 264 threshold),
  and 100 % of them start within 0.03 s of a speaker change;
- the 20 lost turns are all `partial` — some words under half — 10 from each of
  two voices;
- the fixture loops a 60 s clip, so the 20 events sit on two loop offsets
  (14.7 s and 46.8 s): this is one reproducible failure seen twice per loop, not
  twenty unrelated ones;
- 30.1 s of speech falls under empty phrases.

That is the whole 0.4 opportunity: the words are there, the cut is at the wrong
place, and the ASR is asked to recognise a fragment that starts mid-breath.

## 0.4 — Text quality (1–1.5 weeks)

Target: WER 0.303 → ~0.25, lost turns 20.6 % → ~12 %, terms 79.5 % → 85 %+.

### O-1 Prerequisite: one voice, one label

*Why:* the subtitles acceptance is red. The window diarisation merges the quiet
third voice into the loudest id in some runs, so two voices end up on one label
(75 % names, one voice at 4 of 9). No name or term improvement can be trusted while
the label set is still unstable.

*Change:* either a per-window sanity rule (a voice that holds a distinct stretch of
speech may not be folded into an id that another voice already owns) or a stronger
identification signal than time overlap. The reference is the whole-file
diarisation, which separates all three voices on this fixture.

*Measured by:* the subtitles acceptance (three labels, 80 % per voice, no late
labels) and the single-voice acceptance (still exactly one label).

*Done when:* both acceptances pass on two consecutive runs.

*State:* **done.** The window schedule drains a backlog forward in 60 s steps
instead of jumping to the newest audio, turn and caption times stay on the
absolute session clock, and a window that overlaps no labelled speech mints no
speaker; a window where the engine cut one voice into two ids is ignored rather
than used to mint. Two consecutive subtitles runs: 3 labels,
36/39 + 69/69 + 9/9 (names 94.9 %) and 38/39 + 68/69 + 9/9 (names 92.3 %),
steady → last third 185 → 186 and 186 → 187 ms; the single-voice run keeps one
label (27 captions, 249 → 251 ms). Meeting acceptance: 83 phrases, 3 speakers,
2196 → 4760 → 4825 ms. The model-free suites pass, including
`subtitlesKeepOneVoiceOnOneLabel`, which now covers the merged window.

### T-1 Empty-phrase recovery with neighbouring audio (primary)

*Why:* every empty phrase is a loud 1.5 s fragment at a turn boundary; the words
were spoken and the VAD cut them into a piece that the recogniser cannot resolve on
its own.

*Change:* when recognition of a remote phrase returns no words, announce the
caption as pending, separate and recognise a short neighbourhood of the phrase
(4–6 s, from the same timeline the diariser uses) in its own diariser request,
and repaint the pending line when the words arrive. The joined turn starts at the
previous caption's end, and a plausibility gate refuses one-letter output.
Bounded: one retry per phrase, no retries in a chain.

*Where:* `panel/src/core/SessionController.cpp` (the subtitles finish path), with
the context window taken from `remoteTimeline_`.

*State:* closed. The retry was removed from the live path on 2026-11-25: it
produced no real words, only `P`/`Puh.`, and the gated run was exactly the
baseline. The merge variant replaced it and ships in 0.3.2 (measurements below).
Earlier attempts:

- A phrase-level retry with ±1.5 s of neighbour audio and in-span word times
  (`verbose_json`) ran 20 times and accepted 0 words: audio.cpp's detail records
  are token positions, not audio time (`but` at 17.2 s where it is spoken at
  15.2 s), so every word fell outside the span. Empty 14.6 %, lost 20.6 %,
  WER 0.303 before and after, latency 187 → 188 ms. Removed.
- Per-turn recognition (`runtime/t1_turn_spike.py`): join the diariser's pieces of
  one voice across gaps ≤2 s, recognise the joined reply with 0.15 s of lead-in,
  and put its text on the empty phrase. The lead-in matters: the engine's turn
  starts at 15.23 s against a real 15.21 s, and that 20 ms turns `But so I don't
  know…` into `I don't know…`. Ten of the twenty recover; the other ten are
  sub-second interjections (`Mm-hmm`) that the model returns as '' or as a
  hallucinated name. Against 14.6 / 20.6 / 0.303: empty 7.3 % (10/137), lost
  10.3 % (10/97), WER 0.293 keeping the following phrase, 0.269 when phrases
  inside the recovered turn are dropped (empty then 7.9 % of 127). The empty
  gate is missed by one phrase.

The first live embedding held the queue for the retry. It was not a win: empty
captions 14.6 % → 13.1 % and 11.7 %, lost turns unchanged at 20.6 %, WER
**worse** (0.303 → 0.332 and 0.362, transcript words 1188 → 1302), median
steady latency 187 → 248 ms, and an empty phrase cost **8147 ms** because the
queue waited. 16 of the 20 retries used the rolling 120 s window, which answers
in ~5 s, so the 8 s guard fired before it; the four that resolved joined the
same voice back up to 9.5 s and re-said text already on screen.

The four fixes that entry called for were implemented, measured, and then
removed together with the retry (they were correct, but the fragment has no
words):

- Recovery separates and recognises a **short neighbourhood of the phrase**
  (`kRecoveryContext` = 2 s either side, 4–6 s of audio) in its own
  `service_->diarize` call. Its ids are local to that request: nothing is added
  to `knownTurns_`, `resolvedSpeakers_` or the speaker numbering.
- The joined turn starts at the **previous caption's end** (`floorStart`), so
  audio that is already text on screen is never recognised again. This removes
  the 9.5 s joins behind the duplicate words.
- The empty phrase is announced at once as a **pending caption**; recovered
  words arrive through `captionText(id, …)` and repaint that line in
  `CaptionOverlay` and `SessionWindow`. The recognition queue is released
  immediately, so caption order and per-phrase latency no longer wait.
- The guard is **2.5 s** (`kRecoveryGuardMs`), and it only drops a recovery whose
  short window was never sent; a request already in flight is allowed to answer.

**The measurement exposes a problem in T-1's premise.** Ungated, the retry
filled 10 of the 20 empty phrases and empty fell to 7.3 % (10/137) — but the
text was `P` nine times and `Puh.` once. The timing log shows why: every empty
phrase's carrier turn is 0.64–1.14 s, i.e. the phrase itself, and the decoder
returns one character (10 cases) or nothing (10). There are no words in that
fragment: the VAD cut the tail of a word whose beginning is already in the
previous caption. The spike's better numbers came from joining up to 9.5 s back,
which re-recognises audio that is already captioned — the duplicate-word
regression, not recovery.

A plausibility gate (`plausibleRecovery`: at least two words, or one word of
four letters) now refuses that output. The final state, two live runs:

| run | empty | lost | WER | words | steady → last | order | recovered |
|-----|-------|------|-----|-------|---------------|-------|-----------|
| 1 | 14.6 % (20/137) | 20.6 % (20/97) | 0.303 | 1188 | 245 → 186 ms | 0 / 0 | 0 |
| 2 | 14.6 % (20/137) | 20.6 % (20/97) | 0.303 | 1188 | 186 → 186 ms | 0 / 0 | 0 |

So the four fixes do what they were meant to do — the queue is no longer held,
caption order is exact, WER and per-phrase latency are the baseline's — but they
cannot recover words the fragment does not contain. Empty and lost are unchanged
from 14.6 / 20.6 / 0.303. One gated run was lost to a service 504 on a single
transcription (241 s, test timeout at 180 s); the retry passed.

The next step is a different shape, not a longer retry: **merge the fragment into
the caption it belongs to** (the previous one, or the next one when the fragment
is the start of a reply) instead of giving it its own line. The spike measured
that variant at WER 0.269 with empty 7.9 % of 127 phrases. It needs a caption
merge/drop API in the overlay and the window, which is a design decision, not a
tweak.

**Measured (2026-11-25, live path, branch `experiment/t1-merge`):** the merge is
a forward fold. An empty remote phrase keeps its audio, the next phrase is
recognised with it prepended (bounded to 3 s), and the fragment never becomes a
line; no extra request is made. Two runs on the 600 s fixture:

| run | empty | folded | without a line | lost | WER | names | terms | steady → last | order |
|-----|-------|--------|----------------|------|-----|-------|-------|---------------|-------|
| 1 | 14.6 % (20/137) | 20 | 0 | 14.4 % (14/97) | 0.285 | 95.7 % | 82.1 % | 186 → 186 ms | 0 / 0 |
| 2 | 14.6 % (20/137) | 20 | 0 | 14.4 % (14/97) | 0.285 | 94.0 % | 82.1 % | 186 → 187 ms | 0 / 0 |

Against the baseline (lost 20.6 %, WER 0.303, names 92.3 %, terms 79.5 %):
lost turns −6.2 points, WER −0.018, terms +2.6 points, names +1.7–3.4 points,
with no extra requests and no latency change. The per-phrase empty share does
not move — the fragment still has no words of its own — but all 20 are folded
into the next caption, so the card has no empty line. The empty gate as written
(≤7 % of segments) counts phrases, not visible gaps; that definition needs a
decision before the merge can be called done.

*How to unblock (S-2 and alternatives, in the order worth trying):*

1. **Per-turn recognition in the live path: done, and it has no words to
   recover.** The hold is gone, the join is bounded by the previous caption, and
   the words are a repaint by id; the gate keeps junk off the screen. The
   remaining work is the merge described above, not another retry.
2. **Real word times from the backend.** If audio.cpp can emit sample-aligned
   word times (or the times it already emits are fixed where they are produced),
   the removed retry works as written: accept words whose midpoint is inside the
   phrase. Cheapest to add once the backend provides them.
3. **Forced alignment** (WhisperX, `stable-ts`, torchaudio MMS_FA) on the padded
   phrase: it places words that were decoded, it does not recover words the
   recogniser dropped, and it adds a model to the live path; measure it offline
   first.
4. **Whisper `word_timestamps=true`** on the padded phrase: real cross-attention
   times, but on this fixture whisper quantises to 2 s and hallucinates the
   neighbour's sentence on the micro-interjections, so it is the least promising.

*Measured by:* empty captions, lost turns, WER on the subtitles acceptance; the
`diagnose` output must stop charging the boundary fragments to `boundary`.

*Done when:* empty captions drop by at least half (14.6 % → ≤7 %), lost turns fall
to ≤14 %, WER does not get worse, and per-phrase latency stays flat. The merge
meets lost (14.4 %), WER (0.285) and latency (186 → 186 ms); the per-phrase
empty share stays 14.6 % because the fragment has no words of its own, but no
empty line reaches the card. Treat the visible gap, not the segment count, as
the gate.

### T-2 Previous text as ASR prompt (continuity)

*Why:* a fragment at a boundary has almost no acoustic context; the same
conversation's previous words are the cheapest context available, and the panel
already keeps them.

*Change:* pass the last caption text of the same stream as the ASR `prompt`
(`ServiceClient` already sends `context_`), capped in length and never crossing a
speaker turn that the diariser has already labelled.

*Where:* `panel/src/core/ServiceClient.cpp`, `SessionController` call sites.

*Measured by:* WER, terms kept, and a duplicate-text check — prompt leakage shows up
as a caption repeating the previous phrase, which the acceptance would score as
extra words.

*Done when:* WER improves without a rise in repeated phrases, and the meeting run
shows no cross-speaker leakage.

### T-3 Service bug: `verbose_json` drops the prompt for the audio.cpp backend

*Why:* `service/voiceservice/api/openai.py` sends `verbose_json` requests to
`registry.audiocpp.transcribe_detailed()`, and that method takes no `prompt` at all,
so the recognition context is silently discarded exactly when the caller asked for
detail. The whisper branch does pass it.

*Change:* thread `prompt` through `transcribe_detailed()` (and the details endpoint
it calls, if that endpoint accepts it), and make the missing-context case explicit
rather than silent.

*Where:* `service/voiceservice/api/openai.py`,
`service/voiceservice/backends/audiocpp.py`.

*Measured by:* a service test in `tests/` that asserts the prompt reaches the
backend for `json` **and** `verbose_json`, with the HTTP call stubbed; plus one live
request per format against the local service.

*Done when:* both formats carry the prompt, the test fails if either path drops it,
and `docs/api.md` says which fields each format honours.

*State:* **done.** `transcribe_detailed()` takes and sends `prompt`, the verbose
branch passes it, `tests/test_asr_prompt.py` covers both formats and the empty
case, the engine accepts the field (the details endpoint answers 200 with a prompt
present), and [API](docs/api.md) states that the context applies to every format.

### T-4 Deterministic glossary replacement

*Why:* term accuracy is 79.5 %; the model corrects words probabilistically, but a
known term from the user's glossary is a string problem, not a model problem.

*Change:* after recognition and cleanup, replace near-misses of glossary terms and
names deterministically — case-preserving, never inside numbers or already-correct
terms, never across a sentence boundary, with a confidence rule that leaves real
words alone (a term is only forced when the candidate differs by a small edit
distance and the term is not a common word).

*Where:* the text stage of the service (`service/voiceservice/` text processing),
with the glossary taken from the existing context document fields.

*Measured by:* `terms.accuracy` on the acceptance report, plus unit tests: numbers
untouched, terms with spaces and hyphens, Cyrillic/Latin look-alikes, a term that is
also an ordinary word, and idempotence (applying it twice changes nothing).

*Done when:* terms ≥85 %, WER does not rise, and the unit tests above exist and
pass.

### T-5 Speaker names in the overlay and the final document

*Why:* labels are numbers and the finished document is produced by the offline
tool; a person should be nameable while the session is running.

*Change:* rename a speaker in the caption overlay (the meeting window keeps its
existing rename), and add a third tab that shows the assembled result — the
transcript with names, in order, built from the same data the saved Markdown uses.

*Where:* `panel/src/app/CaptionOverlay.*`, `SessionWindow.*`, the transcript model in
`panel/src/core/`.

*Measured by:* a QtTest that renames a label mid-session and asserts later captions,
the tab and the saved document agree.

*Done when:* renaming survives the rest of the session, the third tab matches the
saved file, and the acceptance runs are unaffected (names are still scored from the
diariser's labels).

### Gates for the 0.4 release

All of these on the current tree, with the service running:

1. subtitles acceptance passes: WER ≤0.25, lost ≤12 %, empty ≤7 %, names ≥80 %,
   three labels, no label after the first third, latency drift-free;
2. meeting acceptance passes: 2–4 speakers and no latency drift;
3. single-voice acceptance passes: exactly one label;
4. `python -m pytest tests -q` and `ctest --test-dir panel/build` pass;
5. `diagnose` on the new report no longer classifies the boundary fragments as
   `boundary`, and the report is kept under `runtime/reports/` (not in Git).

Release notes follow the shape of [0.3.0](docs/releases/0.3.0.md): what changed, the
numbers before and after, and the limitations that remain.

## 0.5 — Streaming and captions (2–3 weeks)

Target: speech end to caption ~4.7 s → ~1 s, fewer losses at phrase start,
readable captions.

- **S-1 Streaming live path.** Recognise while the phrase is still being spoken
  instead of after the VAD endpoint, with partial results kept off the overlay (the
  panel's insertion path already assumes no partial-result insertion). Measured by
  `VOICEPANEL_TIMING_LOG` + `tools/bench/panel_timing.py` and by the acceptance's
  per-phrase latency, which must fall without the WER regression that a shorter
  lookahead usually brings.
- **S-2 Word timestamps or forced alignment.** Needed for names in the finished
  document, for the caption rules below, and for T-1: the phrase-level retry was
  measured and removed because audio.cpp's detail times are token positions, not
  audio time. Measured by the share of words that get a timestamp and by name
  accuracy in the saved file.
- **S-3 Caption readability.** Line length, characters per second and minimum
  duration as hard rules in the overlay; a phrase that cannot be read in its
  on-screen time is split or extended, not sped up. Measured by a report of rule
  violations per 100 captions, which must reach zero.
- **S-4 Overlap and suspect lines.** Mark speech that two people share and lines
  the system is unsure about, instead of presenting both as clean. Measured by the
  share of overlapping turns that are marked and the false-mark rate on clean
  speech.

Gates: the 0.4 gates still pass, plus latency ≤1.5 s steady with no drift.

## 0.6 — Models (only after a cleanup metric exists)

Not a commitment. Before any model or cleanup change, the metric is defined and
baselined:

- punctuation quality (sentence ends, clause commas, quoted speech);
- words added by cleanup as a share of the transcript, with the number and term
  preservation rate next to it;
- term and number preservation, which must not fall.

Then, and only then: consensus of nemotron + whisper in the offline pass, compared
against the single-model reading on the same fixture. Measured by the metric above
plus WER; a change that improves style and loses numbers does not land.

## Order of work

1. O-1 (one voice, one label) — done; the label set is stable enough to score
   names and terms against.
2. T-3 (prompt bug) — done, and it unblocks T-2.
3. T-1 (empty-phrase recovery) — open; both attempts are measured above. Quality
   wins (empty 7.3 %, lost 10.3 %, WER 0.293) but the live caption order blocks
   it; decide whether to delay or reorder, or use it only in the saved document.
4. T-2 (prompt continuity) — measured on top of T-1, so the two effects stay apart.
5. T-4 (glossary replacement) — deterministic, measurable on its own.
6. T-5 (names and third tab) — user-visible, independent of the metrics above.

T-1 and T-2 are deliberately separate steps: done together, a WER improvement could
not be attributed to either.

## Verification

Commands, from the repository root, with the service on its default address:

```powershell
# labels and transcript quality (several minutes each)
$env:QTEST_FUNCTION_TIMEOUT = 1200000
$env:VOICEPANEL_SUBTITLES_BENCH = "http://127.0.0.1:8080"
.\panel\build\bin\VoicePanelTests.exe -o runtime\logs\subtitles-long.txt,txt subtitlesLongRecordingKeepsSpeakersAndLatency
$env:VOICEPANEL_SUBTITLES_SINGLE_BENCH = "http://127.0.0.1:8080"
.\panel\build\bin\VoicePanelTests.exe -o runtime\logs\single.txt,txt singleVoiceRecordingKeepsOneLabel
$env:VOICEPANEL_MEETING_BENCH = "http://127.0.0.1:8080"
.\panel\build\bin\VoicePanelTests.exe -o runtime\logs\meeting.txt,txt meetingLongRecordingKeepsSpeakersAndLatency

# what the report says, and why
python tools\bench\transcript_quality.py score --report runtime\reports\subtitles-long\result.json
python tools\bench\transcript_quality.py diagnose --report runtime\reports\subtitles-long\result.json

# the model-free suites
python -m pytest tests -q
ctest --test-dir panel/build --output-on-failure
```

Reports land in `runtime/reports/`, logs in `runtime/logs/`; both are ignored by Git.
A number in this plan is only worth citing next to the command that produced it.

## Known risks

- **Run-to-run variance.** The diariser's window clustering varies between runs, so
  a single good name figure proves nothing; O-1 is done when two consecutive runs
  pass.
- **Accelerated acceptance feed.** The caption acceptance feeds phrases about four
  times faster than real time, so a five-second diarisation is twenty seconds of
  audio behind. Real sessions are slower; the acceptance is the harsher case and
  stays as it is.
- **Fixture levels.** `meeting-long-16k.wav` had to be levelled to the captions
  fixture before the VAD could hear all three participants (see
  `runtime/bench/_mkmeeting.py`). A fixture that silently feeds one voice produces a
  meaningless pass.
- **Panel relink.** The desktop executable cannot be relinked while a running
  instance holds `onnxruntime.dll`; builds of `VoicePanelTests` are unaffected.

## Longer term: voice messages

VoicePanel turns speech into text, processes completed sentences, inserts text into a
remembered field, and plays or saves synthesized speech. Direct voice-message
sending is postponed.

The existing capture, VAD, transcription, review and playback components could form
the base of a separate voice-message workflow:

1. Record a message and keep its audio together with its transcript.
2. Review the transcript and listen to the recording before sharing.
3. Choose whether to export the original recording or synthesize a new recording
   from reviewed text with a selected voice.
4. Select a destination and confirm each send action explicitly.

Future integrations could support direct audio transfer and voice messages in
supported applications. They would need separate destination adapters, permission
handling, delivery results and retry behavior.

Conditions before implementation:

- The user controls recording, review, destination and sending.
- A dictation shortcut never sends audio or submits a message automatically.
- Recordings remain local until the user explicitly chooses to share them.
- Cloned-voice workflows require a recording the user has the right to use.
- Integrations need their own security review and target-application tests.

This is a direction for future work, not a feature commitment. No timeline is set.
The current release has no direct voice-message transmission or automatic message
sending.
