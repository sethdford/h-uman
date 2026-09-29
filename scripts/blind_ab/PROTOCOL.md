# Blind Human A/B — The Humanness Ground Truth

The eval suites (`human_likeness`, `adversarial_turing`, …) are **proxies**: LLM judges
scoring single-turn tasks. They told us h-uman scores 0.88–1.00 — but they cannot see
whether the people who actually know Seth can tell the difference. **This protocol is the
ground truth.** Everything else is a stand-in for this number.

## The measurement

A **two-alternative forced choice (2AFC)**. For each real conversation context, a rater
sees two replies — one written by Seth, one by h-uman — unlabeled and in random order —
and answers: *"Which is more like Seth?"* plus a confidence 1–5.

- **Detection rate** = fraction of items where the rater correctly picks **Seth's real reply**.
- **0.50 = indistinguishable** (the goal). **1.00 = always caught.** Chance is the win.
- Target: **detection ≤ 0.60 and trending toward 0.50**, with a 95% CI that includes 0.50.

This is the number the whole coherence plan (`plans/2026-05-30-coherence-arbitration.md`)
is trying to move. Run it BEFORE arbitration (baseline) and AFTER (did it help?).

## Inputs you provide

A `triples.json` — a list of items, each:

```json
{ "id": "t001",
  "context": "the inbound message (and optional prior turns) Seth was replying to",
  "seth_reply": "what Seth actually sent",
  "huuman_reply": "what h-uman generates for the same context" }
```

- 10–15 items per rater minimum (120+ total votes across raters = a usable CI).
- Use **real** contexts and Seth's **real** replies. The h-uman reply is generated for the
  same context (see README for the generation step). Garbage-in here invalidates everything.

## Raters

- **5–8 people who genuinely know Seth** — read his texts, know his voice, timing, humor.
- Strangers cannot run this test; familiarity is the instrument.
- They rate independently, no discussion, no labels, no peeking at the answer key.

## Procedure

1. `make_rating_sheet.py triples.json` → a shuffled `rating_sheet.csv` (no labels) + a
   private `answer_key.json` (you keep this; raters never see it).
2. Send each rater the sheet + `RATER_INSTRUCTIONS.txt`. They fill `choice` (A/B) and
   `confidence` (1–5) for every row.
3. Collect completed sheets → `score.py rating_sheet_<rater>.csv ... --key answer_key.json --rater human`
   (`--rater human` marks the promotion-authoritative gate write; machine-judged
   sheets use `--rater synthetic`, which records under a separate key).

## Pass criteria (both must hold)

1. **Detection rate ≤ 0.60**, 95% Wilson CI lower bound ≤ 0.55 (can't confidently beat chance).
2. **Confidence-weighted detection ≤ 0.60** (high-confidence catches count double; a bot
   that's caught only on low-confidence guesses is still doing well).

If detection is already near 0.50 today, the felt "5%" gap is NOT discrimination — it's
something else (and the arbitration build should be re-justified). If detection is high
(≥0.75), the gap is real and located, and arbitration has a number to move.

## Anti-gaming notes

- **Never tune on the proxy** (heuristic Turing / ai-tells). Tune toward THIS number only.
  (Risk R12 in the plan.)
- Rotate contexts between baseline and post-arbitration runs so raters don't memorize items.
- Keep raters blind to which arm (on/off) they're rating.

## Preference measurement (win rate) [US-6]

**This is a SEPARATE measurement from the detection protocol above.** It asks a
different question, is scored by a different script, and never touches the
promotion-authoritative gate (`~/.human/blind_ab_gate.json`,
`docs/evaluation/blind_ab_gate.json`). Never mix the two framings on one sheet.

### The question

For each item, the rater sees the same two unlabeled replies as the detection
protocol -- but answers a different question: **"Which of these two replies
would you rather RECEIVE from Seth -- not which one IS Seth, which one do you
PREFER?"** Confidence 1-5, same column, same meaning.

- **Win rate** = fraction of items where the rater's pick matches the MODEL
  (h-uman) reply, not Seth's real one.
- **0.50 = a coin flip** between the two. **> 0.50 = the model reply is
  preferred over Seth's own reply** -- a genuinely harder bar than detection's
  "indistinguishable," and the whole point of measuring it separately.
- A win rate below 0.5 is an acceptable, honestly-recorded outcome. This
  protocol does not retry sampling or reframe questions until the number
  looks better (AC-6.6).

### Procedure

1. `make_rating_sheet.py triples.json --mode preference --out-dir <path outside
   the repo>` -- reuses the same exported `triples.json` as detection (same
   `context`/`seth_reply`/`huuman_reply`), just a different sheet framing.
   Redacts phone-number-shaped and contact-name-shaped substrings from every
   field before writing (same redaction as the detection sheet, applied in
   both modes). Any triple whose `seth_reply` and `huuman_reply` are identical
   after redaction has no "model side" to prefer and is excluded, with the
   skip count printed.
2. Send each rater the sheet + preference-framed instructions (this section,
   or a sibling `RATER_INSTRUCTIONS_PREFERENCE.txt`). They fill `choice`
   (A/B) and `confidence` (1-5) for every row -- same mechanics as detection,
   different question.
3. `score_preference.py rating_sheet_<rater>.csv --key answer_key.json --rater
   human --evidence-out sprints/<sprint>/evidence/US-6/preference-results-<date>.json`
   -- reuses `score.py`'s `wilson()`/`score_rows()` math UNMODIFIED to compute
   win rate + 95% CI. Refuses (exit non-zero, writes nothing) below n=20
   pairs, for a non-`human` rater tag on an evidence-writing invocation, or
   against a key that isn't stamped `"_mode": "preference"`.

### What this is NOT

- **Not a gate.** No LLM judge is ever in the verdict path, and this script
  has no code path that touches either blind-A/B gate file. It is a
  measurement, not a promotion decision.
- **Not the detection protocol.** The 0.60/0.55/0.60 detection thresholds
  above are completely untouched by this section; a sheet built with
  `--mode preference` cannot be scored by `score.py` or vice versa without an
  explicit refusal (the `_mode` marker enforces this).

### Known limitation

Redaction is regex (phone-shaped and email-shaped, the latter replaced with
`<redacted-email>`) + a real contact-name-token match, not a general PII
scrubber -- an unusual phone format, an address with no dotted domain, or a
nickname absent from the AddressBook can slip through. A human should skim a generated sheet before
sending it to raters, the same manual judgment the existing recruitment step
already requires.
