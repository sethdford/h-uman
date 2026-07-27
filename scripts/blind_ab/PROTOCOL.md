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
