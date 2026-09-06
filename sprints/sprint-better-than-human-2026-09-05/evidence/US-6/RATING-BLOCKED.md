# US-6 Preference Blind A/B — Harness Done, Real Run Blocked on Raters

## What the agent completed

- [x] `scripts/blind_ab/make_rating_sheet.py --mode preference` (default remains
      `detection`, byte-for-byte backward compatible for every existing caller)
- [x] `redact()` — phone-number-shaped and contact-name-shaped substrings stripped
      from `context`/`seth_reply`/`huuman_reply` in BOTH modes (AC-6.4; this closed a
      real pre-existing gap in the detection sheet too, per designs/US-6.md)
- [x] Real/real duplicate-pair exclusion in preference mode, with printed skip count
      and a hard refusal (exit non-zero, nothing written) if it empties the sheet
- [x] `scripts/blind_ab/score_preference.py` — reuses `score.py`'s `wilson()` /
      `score_rows()` math **unmodified** (AC-6.2); refuses on n=0, n<20 (AC-6.5's own
      floor), a non-`human` rater tag on an evidence-writing invocation (AC-6.3), and a
      key file not stamped `"_mode": "preference"`
- [x] Zero code path in `score_preference.py` touches `~/.human/blind_ab_gate.json` or
      `docs/evaluation/blind_ab_gate.json` — this is a measurement, never a gate
      (out of scope per stories.md:322-323)
- [x] `scripts/blind_ab/PROTOCOL.md` — new "Preference measurement (win rate)" section,
      clearly separated from the existing detection section (AC-6.1)
- [x] Hermetic tests: `scripts/blind_ab/test_make_rating_sheet.py` (27 tests: redaction,
      `--mode preference` key shape, duplicate-pair skip/refusal, CLI end-to-end) +
      `TestPreferenceScoring` in `scripts/blind_ab/test_score.py` (9 tests: n=0, n<20,
      synthetic-rater no-write, stamped-sheet veto, missing/wrong `_mode` refusal, a
      synthetic 15/20 sheet matching `wilson(15, 20)` exactly, and a full human-run
      evidence-schema + never-touches-the-gate-file check). All pass; no network, no
      real AddressBook read (`HU_BLIND_AB_SKIP_ADDRESSBOOK=1`), no `~/.human` writes.

## What blocks the real n>=20 run (AC-6.5)

The harness is complete and independently testable. **Agent work is done.** Producing
the committed `preference-results-<date>.json` requires the same human-in-the-loop
steps as the existing detection protocol's `sprints/sprint-1/evidence/US-4/RATING-BLOCKED.md`
— reusing the SAME exported `triples.json` this design assumes (see designs/US-6.md
"Rater flow" step 4), not a fresh `chat.db` export:

1. **A `triples.json` of real `{id, context, seth_reply, huuman_reply}` items** —
   already-exported triples from a prior detection cycle can be reused directly; no
   new privacy-sensitive export is required.
2. **5-8 raters who genuinely know Seth** (same pool PROTOCOL.md's detection section
   specifies) — confirm availability for a SECOND question pass over the same items
   (the open question in stories.md:332-334, explicitly left unresolved by
   designs/US-6.md).
3. Generate the preference sheet:
   ```bash
   python3 scripts/blind_ab/make_rating_sheet.py <triples.json> --mode preference \
     --out-dir ~/blind_ab_preference_run/    # OUTSIDE the repo, matches ~/.human/blind_ab_human/
   ```
4. Distribute `rating_sheet.csv` + the preference-framed instructions (PROTOCOL.md's
   new section) to raters. They fill `choice` (A/B, "which would you rather receive")
   and `confidence` (1-5).
5. Once >=20 pairs are answered by a human rater, score and commit the evidence:
   ```bash
   python3 scripts/blind_ab/score_preference.py \
     ~/blind_ab_preference_run/rating_sheet_<rater>.csv \
     --key ~/blind_ab_preference_run/answer_key.json \
     --rater human \
     --evidence-out sprints/sprint-better-than-human-2026-09-05/evidence/US-6/preference-results-$(date +%F).json
   ```
   This is the ONLY file this story ever commits from a real run: `{n, win_rate,
   ci_lo, ci_hi, rater, date}` — no message text, no phone numbers, no names
   (AC-6.5's "raw sheet text itself is never committed").

## AC-6.6 reminder for whoever runs this

A win rate below 0.5 is an acceptable, honestly-recorded outcome. Do not re-sample,
reframe the question, or discard low results to chase a better number — the design
and this story's tests treat "win_rate=0.30, n=22" as exactly as valid a result as
"win_rate=0.70, n=22". Commit whatever `score_preference.py` reports.

`AGENT_COMPLETED=YES  RATING_BLOCKED_ON_USER=YES`
