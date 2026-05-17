# Sprint 29 — catch CoT / prompt-context leaks in `response_guard.c`

## Why now

On 2026-05-12 at 17:08 UTC-4, the live h-uman service-loop daemon
sent a 979-byte chain-of-thought / prompt-context dump to a real
human contact (`+14848158444`). The user (h-uman owner) deleted
it from his iMessage UI before it could be screenshotted by the
recipient, then asked us to dig.

Service-loop log line 7351 captured the failure mode:

    [agent_stream] response_guard REWROTE: stripped 82 stream bytes
    [agent_stream_v2] quality gate MARGINAL: score=53 — Your response
                       was 979 chars but their last messages averaged
                       44 chars. Tighten up significantly.
    [human] agent turn result: err=ok response_len=979

The primary `hu_response_guard_check` returned `REWROTE` after
stripping 82 bytes (probably the leading whitespace and an
artifact of the trailing-trim) — letting 897 bytes of model
self-narration pass to the channel. The downstream quality gate
flagged "MARGINAL" at score 53, but `MARGINAL` does not block,
only `REJECTED` does. The 22x length anomaly (979 chars vs 44
char rolling average) was visible to the gate but treated as
advisory.

Sprint 29 hardens the **primary guard** to recognize the failure
shape and emit `REJECT` instead of `REWROTE`. Once the primary
emits `REJECT`, the existing slim-retry flow (with its in-flight
WIP context-aware repair prompt in main worktree) takes over and
generates a clean reply. Length-anomaly hard-block and director-
echo detection are deliberately deferred to a follow-on sprint
because they require threading new context through the guard's
public API — that touches every caller and is a bigger change
than the safety patch the user needs today.

## The leaked payload (verbatim, line 7353 hex-decoded)

```
 A link (presumably to a business or quote).
        2.  "King Carpet and Flooring" (Business name).
        3.  "Noon tmr" (Appointment time).
        4.  Instruction to ignore a "consumer notice" question.
        5.  Venmo handle (@fegofficial) and a price ($25) for a photographer today.

 Seth is a technical professional, lives alone with a cat.
 He's talking to Brea (romantic interest, casual, early stage).
 The conversation has suddenly pivoted to logistics (carpet repair,
 consumer notices, photographers). This feels like a mix-up or a
 very specific coordinated effort.
 Seth just "glitched" in the previous message, admitting he was off-track.
 Now the user is bombarding him with logistics.

 Professional, slightly skeptical (per scene direction, though that
 was for a previous prompt, I should still maintain the persona).
 Wait, the prompt says "Professional, slightly skeptical, ask for
 clarification on why they"
```

Why every existing phase missed it:

| Phase | Looks for | Why it missed |
| --- | --- | --- |
| 0 | `* ` asterisk-bullet prefix | Leak uses `1.` numbered, not `*` |
| 1 | `<\|...\|>`, `<think>`, `<thought>` | Plain prose, no markup |
| 2 | char run > 35, token run > 8 | Varied vocabulary, no repetition |

## Stories

### G1. Phase 0 extended — numbered analytical-list dump

`src/agent/response_guard.c` Phase 0 currently fires only when
the response begins (after leading whitespace) with `* `. Extend
the same code path so it also fires when the response begins
with one of:

- `1. ` (digit, period, space)
- `1.  ` (digit, period, two spaces — the actual leak shape)
- `1) ` (digit, paren, space)

When detected, walk forward line-by-line. A line is "still
analysis" when:

- it starts with a digit followed by `.` or `)` followed by
  whitespace, OR
- it starts with whitespace (`  `, `\t`, etc.) followed by a
  digit, period, paren — the indented-continuation shape, OR
- it's blank.

The first line that starts with an alphanumeric or punctuation
*and is not* one of the above is the real reply. If no such line
exists, the entire response is analysis → REJECT.

This catches the leak's `1. ... 2. ... 3. ... 4. ... 5. ...`
opening, walks past the indented continuations, hits the blank
line, then the `Seth is a technical professional` line which is
the start of the third-person profile — that line then trips G3
and the guard rejects.

### G2. New Phase 1.5 — model self-talk / scene-direction echo

A short list of substring patterns (case-insensitive) that have
**no legitimate human-to-human reply use case**:

| Pattern | Caught from leak |
| --- | --- |
| `the prompt says` | line "Wait, the prompt says..." |
| `wait, the prompt` | same |
| `i should still maintain` | "I should still maintain the persona" |
| `(per scene direction` | "(per scene direction, though..." |
| `per the scene direction` | (broader variant) |
| `the user is bombarding` | "Now the user is bombarding him with logistics" |
| `the user is asking` | (broader variant — model labelling the human as "the user") |

Implementation: a single static array of `{ pattern, len }` pairs
walked once with `hu_strcasestr`-equivalent (we'll write a
minimal one — case-insensitive memmem). **Single match → REJECT.**
No second-chance, no rewrite. These patterns are aggressive on
purpose; their false-positive rate against legitimate human
replies is essentially zero.

### G3. New Phase 1.5 — third-person-about-the-user double-pattern

Single hits on third-person patterns are ambiguous (a real reply
might say "He's coming over later"). Require ≥2 distinct
patterns matching in the same response:

| Pattern shape | Example from leak |
| --- | --- |
| `\b[A-Z][a-z]+ is a` followed within 60 chars by one of `technical`, `professional`, `software`, `chief`, `data`, `product`, `senior`, `engineer`, `manager`, `architect`, `scientist`, `developer` | "Seth is a technical professional" |
| `\bhe's talking to ` / `\bshe's talking to ` / `\bthey're talking to ` (case-insensitive, word boundary) | "He's talking to Brea" |
| `\b[A-Z][a-z]+ just "` (proper-name self-narration with quoted action) | "Seth just \"glitched\"" |
| `\blives alone with` / `\blives with his` / `\blives with her` (biographical) | "lives alone with a cat" |
| `\bromantic interest, casual` / `\bromantic interest, early stage` (relationship-stage labelling) | "(romantic interest, casual, early stage)" |

Walk these once, count distinct hits. **count ≥ 2 → REJECT.**
The leak hits 5 of these — well past threshold.

### G4. Tests

In `tests/test_response_guard.c`, add five tests:

1. **`response_guard_rejects_2026_05_12_brea_leak_verbatim`** —
   the verbatim 979-byte payload from `service-loop-error.log:7353`.
   Asserts outcome is `HU_GUARD_REJECT` and `*out_response` is NULL.
2. **`response_guard_rejects_numbered_analysis_dump`** — synthetic
   `1.  X.\n        2.  Y.\n        3.  Z.\n` with no reply tail.
   Asserts REJECT.
3. **`response_guard_rejects_self_talk_substrings`** — six
   sub-cases, one per G2 pattern, each in a one-sentence reply.
   Each sub-case asserts REJECT.
4. **`response_guard_rejects_third_person_double_pattern`** —
   one input with 2 G3 hits → REJECT; one input with only 1 G3
   hit (e.g. `"He's talking to me about it"`) → NOT REJECT
   (passes through to OK or REWROTE).
5. **`response_guard_passes_legit_replies_with_similar_surface_features`**
   — negatives:
   - `"Wait, what time tomorrow?"` (legit "Wait, ", no "the prompt")
   - `"He's talking to me later"` (single G3 hit, not double)
   - `"1. dinner 2. movie"` (numbered list but no analytical
     suffix — actually this WILL trip G1 walking; needs a real
     reply tail to pass; let's use `"1. dinner 2. movie\n\nWyd?"`
     so the trailing `Wyd?` line is the reply)

## Definition of Done

- All five new tests pass.
- No regression in existing `tests/test_response_guard.c`.
- Dev build: 10,222 + 5 = **10,227** tests pass.
- Minimal build: 9,877 + 5 = **9,882** tests pass.
- 0 ASan errors in both builds.
- `git tag v-sprint-29-close`.
- `sprints/sprint-29/review.md` + `retro.md`.

## Out of scope (deliberately)

- **Length anomaly hard-block** — requires `recent_avg_len`
  parameter through the guard API. Worth doing; bigger sprint.
- **Director-string echo** — same; needs the director string
  threaded through.
- **Quality gate MARGINAL→ship policy fix** — separate file
  (`agent_stream_v2.c` quality gate), separate code path. Sprint
  29 is the primary guard.
- **Existing retry generation WIP in main worktree** — left
  alone, complementary.
- **Post-mortem doc** — `docs/postmortems/2026-05-12-brea-cot-leak.md`
  belongs in a separate doc-only PR for traceability.
