# US-4 Human-in-the-Loop Blocking Dependencies

## What the agent completed

- [x] Harness wiring (gen → sheet → score pipeline) — all three Python scripts verified end-to-end
- [x] End-to-end test with synthetic data (5 triples, dummy ratings)
- [x] Verification that all three scripts run cold and produce expected outputs
- [x] Added `--json-out` flag to `score.py` for programmatic JSON output
- [x] PROTOCOL.md and RATER_INSTRUCTIONS.txt are documented and ready

## What blocks real blind A/B rating (AC-4.5)

The harness wiring is complete. **Agent work is done.** The following human-in-the-loop steps must be completed before real rating can proceed.

### 1. Seth's real sent-message export (REQUIRED FIRST)

**Deliverable:** ~100–200 representative iMessage conversations with real replies.

**What to provide:**
- A JSON file containing message triples from your actual chat.db
- Each item must have three fields:
  - `contact_name`: who you're talking to (e.g., "mom", "Alex", "work-slack")
  - `context`: the actual message you received (the prompt you're replying to)
  - `seth_reply`: the actual reply you sent (the ground truth)
  
**How to export — ONE command (tool added 2026-05-31):**
```bash
# The real iMessage DB is at ~/Library/Messages/chat.db (NOT ~/.human/chat.db).
# Modern macOS stores message text in attributedBody (binary), so a plain SQL
# dump returns nothing — this script decodes it for you.
python3 scripts/blind_ab/export_seth_triples.py --limit 200 --out seth_triples.json

# If you hit "permission denied" / DB locked: either grant your terminal
# Full Disk Access (System Settings > Privacy & Security > Full Disk Access),
# or copy the DB first:
cp ~/Library/Messages/chat.db /tmp/chat.db && \
  python3 scripts/blind_ab/export_seth_triples.py --db /tmp/chat.db --out seth_triples.json
```
Output: a JSON array of `{contact_name, context, seth_reply}` with your REAL
replies. Contact handles are aliased (contact_1, contact_2, …) by default so no
phone numbers reach raters. Smoke-tested: 200 pairs, 0 decode failures.

**Diversity tip:** the script fills oldest→newest per chat, so a single big
thread can dominate. For emotional/contextual range across people, run it once
per important contact with `--keep-handles` into separate files and concatenate,
or raise `--limit` and hand-trim. The blind A/B is strongest when triples span
multiple contacts and moods.

**Why:** The entire measurement validity depends on using Seth's REAL replies. Synthetic or made-up conversations invalidate the ground truth and the blind A/B rating loses meaning.

**Timeline:** Before rater recruitment. This is blocking.

**Acceptance criteria:**
- JSON file with ≥100 items
- Each item has `{contact_name, context, seth_reply}` fields
- Triples span multiple contacts and time periods (emotional range, conversational styles)
- No synthetic or example data mixed in

### 2. Rater recruitment and training (REQUIRED SECOND)

**Deliverable:** 5–8 raters who know Seth personally.

**Who qualifies:**
- People who have texted Seth regularly (minimum 6+ months)
- People who have spoken with Seth in person or on calls (familiarity with his voice/tone)
- People who can commit 1–2 weeks to rate 10–20 items per person
- People who can follow instructions without gaming the test (no communication between raters, no peeking at answer key)

**Training materials ready:**
- `scripts/blind_ab/RATER_INSTRUCTIONS.txt` — raters MUST read this before rating
- `scripts/blind_ab/PROTOCOL.md` — the full measurement contract (for context, not required reading)

**How to recruit:**
1. Identify 5–8 candidates who meet the "who qualifies" criteria above
2. Send each candidate:
   - RATER_INSTRUCTIONS.txt (explain what they're rating and why)
   - Their personalized rating_sheet.csv (will be generated once you provide the triples)
   - Timeline: ask for ratings back within 7–10 days
3. Emphasize:
   - Blind rating only — they should NOT discuss their ratings with other raters
   - They will NOT see the answer key until after all ratings are submitted
   - The goal is to measure whether Seth's replies are indistinguishable from AI (not to find AI — they're trying to pick Seth and failing is success)

**Timeline:** After export (step 1) and before sheet generation. Parallel OK with step 1.

**Acceptance criteria:**
- 5–8 confirmed participants
- Each has read RATER_INSTRUCTIONS.txt
- Each understands the rating task (2AFC: pick which option is Seth's real reply)
- Each committed to returning ratings within 7–10 days

### 3. Sheet generation (REQUIRED THIRD, AGENT-COMPLETABLE IF TRIPLES PROVIDED)

**Deliverable:** rating_sheet_<rater>.csv per rater.

**What to do:**
Once you have the real triples (step 1), you can run:
```bash
python3 scripts/blind_ab/make_rating_sheet.py \
  <your-exported-triples.json> \
  --seed 42 \
  --out-dir sprints/sprint-1/evidence/US-4/

# This produces:
#   rating_sheet.csv (the sheet to distribute)
#   answer_key.json (KEEP PRIVATE — never show to raters)
```

If you have multiple raters and want per-rater sheets (optional, for tracking), generate one per:
```bash
python3 scripts/blind_ab/make_rating_sheet.py \
  <triples.json> \
  --seed $(python3 -c "import hashlib; print(int(hashlib.md5(b'alice').hexdigest(), 16) % 10000)") \
  --out-dir sprints/sprint-1/evidence/US-4/alice/
```

**Timeline:** After steps 1 and 2.

**Acceptance criteria:**
- rating_sheet.csv exists with ≥100 rows (one row per item)
- answer_key.json exists and is KEPT PRIVATE (do not send to raters)
- CSV header: id, context, option_A, option_B, choice, confidence
- Each row has context + two options (unlabeled, randomized A/B order)
- choice and confidence columns are empty (raters fill these in)

### 4. Rating completion (REQUIRED FOURTH — HUMAN EFFORT)

**Deliverable:** Completed rating_sheet.csv from each rater.

**What raters do:**
For each row in the sheet:
- `choice`: write A or B (which option is Seth's REAL reply, in your judgment?)
- `confidence`: write 1 (guess) to 5 (very confident)
- Return the CSV to you

**Timeline:** Distribute sheets after step 3, collect back over 1–2 weeks.

**Acceptance criteria:**
- All ≥100 items have choice ∈ {A, B} filled
- All ≥100 items have confidence ∈ {1, 2, 3, 4, 5} filled
- Raters returned sheets independently (no communication between them)
- All ≥5 raters completed their sheets

### 5. Score aggregation and verdict (AGENT-COMPLETABLE ONCE SHEETS RETURNED)

**Deliverable:** Final verdict: PASS or FAIL.

**What to do:**
Once all raters have returned their completed sheets, run:
```bash
python3 scripts/blind_ab/score.py \
  sprints/sprint-1/evidence/US-4/rating_sheet_alice.csv \
  sprints/sprint-1/evidence/US-4/rating_sheet_bob.csv \
  sprints/sprint-1/evidence/US-4/rating_sheet_charlie.csv \
  ... (all rater sheets) \
  --key sprints/sprint-1/evidence/US-4/answer_key.json \
  --json-out sprints/sprint-1/evidence/US-4/FINAL_blind-ab-results.json
```

**What the results mean:**
- `PASS` → detection rate ≤ 0.60 AND Wilson CI lower ≤ 0.55 AND confidence-weighted ≤ 0.60
  - Raters could NOT consistently distinguish Seth's replies from AI. **Success: h-uman is indistinguishable.**
  - Stories US-1 (GraphRAG) and US-3 (Salience) can proceed to prod activation.
- `FAIL` → any of the above thresholds exceeded
  - Raters COULD distinguish Seth's replies. **Gap identified: h-uman needs improvement.**
  - Do NOT activate Stories US-1 or US-3 until quality improves; run another blind A/B round after improvements.

**Timeline:** Same day all sheets are returned, ~5 min to run.

**Acceptance criteria:**
- `score.py` runs without error
- FINAL_blind-ab-results.json contains numeric scores ∈ [0.0, 1.0]
- Verdict is PASS or FAIL (printed in stdout and in JSON)
- Decision is documented (who was the decision-maker, when, any notes on next steps)

---

## Gating Story Decisions: A/B and C

**CRITICAL: Do NOT activate Stories US-1 or US-3 until this story's verdict is received.**

- If PASS: h-uman is indistinguishable from Seth's real replies. Proceed with GraphRAG activation (US-1) and Salience activation (US-3).
- If FAIL: h-uman still has a perceptible gap. Spend time improving reply quality before attempting activation. Return here for a second blind A/B round.

This is the ONLY gate on production activation. No exceptions.

---

## Summary of User Responsibilities

| Step | Owner | Deliverable | Timeline |
|------|-------|-------------|----------|
| 1. Export triples | You | ~100–200 real {contact, context, reply} items as JSON | Before step 2 |
| 2. Recruit raters | You | 5–8 confirmed participants who read instructions | Before step 3 |
| 3. Generate sheets | Agent or you | rating_sheet.csv per rater (generated via Python script) | Before step 4 |
| 4. Rate items | Raters | Completed rating_sheet.csv (choice + confidence filled) | 1–2 weeks |
| 5. Score & decide | Agent or you | FINAL verdict: PASS or FAIL (generated via Python script) | Same day sheets return |

**Total time from agent completion to decision:** 2–4 weeks (mostly waiting for raters).

**Next steps:**
1. Follow step 1 above: export your chat.db triples
2. Run step 2: recruit raters
3. Run step 3 (Python script is ready) to generate sheets
4. Distribute to raters
5. Once all sheets return, run step 5 to get the verdict
6. Document the decision in this file and proceed with Stories US-1 and US-3 (if PASS)

---

## Appendix: Advanced Options

### Generating per-rater sheets with deterministic seeding (optional)

If you want each rater to see the items in a different shuffled order (to prevent coordination), you can generate per-rater sheets with deterministic different seeds:

```bash
for rater in alice bob charlie; do
  seed=$(python3 -c "import hashlib; print(int(hashlib.md5(b'$rater').hexdigest(), 16) % 100000)")
  python3 scripts/blind_ab/make_rating_sheet.py \
    <triples.json> \
    --seed $seed \
    --out-dir sprints/sprint-1/evidence/US-4/$rater/
done
```

Each rater gets the SAME items but in different order (same answer_key, shared).

### Auditing completed sheets before scoring

If you want to verify all sheets are complete before running score.py:

```bash
python3 << 'AUDIT'
import csv, sys
for path in sys.argv[1:]:
    with open(path) as f:
        r = csv.DictReader(f)
        rows = list(r)
    missing_choice = [row['id'] for row in rows if not row.get('choice', '').strip()]
    missing_conf = [row['id'] for row in rows if not row.get('confidence', '').strip()]
    if missing_choice or missing_conf:
        print(f"{path}: missing choice {missing_choice}, missing confidence {missing_conf}")
    else:
        print(f"{path}: ✓ complete ({len(rows)} items)")
AUDIT
```

---

`AGENT_COMPLETED=YES  RATING_BLOCKED_ON_USER=YES`
