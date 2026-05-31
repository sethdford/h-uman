---
title: "Design — US-4: Wire and run blind A/B rating harness (HUMAN-IN-THE-LOOP)"
sprint: 1
story: "US-4"
created: 2026-05-30
status: ready_for_implementation
authored_by: tech-lead
---

# Design for US-4: Wire and run blind A/B rating harness

## Executive summary

The blind A/B harness **exists in working form** at `scripts/blind_ab/`. Four entry points are already written and testable:

1. `gen_huuman_replies.py` — generates h-uman replies for a list of contexts (reads from eval runner's persona + iMessage path, writes to eval_results table)
2. `make_rating_sheet.py` — converts triples into a shuffled 2AFC rating sheet + private answer key
3. `score.py` — computes detection rate, Wilson CI, per-rater breakdown, and verdict
4. `PROTOCOL.md` + `RATER_INSTRUCTIONS.txt` — documented measurement contract and rater guidance

**What this story delivers:** end-to-end wiring verification with synthetic test data, demonstrating that the full pipeline runs cold from input JSON through final scoring. A successful test run proves AC-4.1 through AC-4.6 are achievable. The only blocking dependency is **AC-4.5's human-in-the-loop**: Seth's real sent-message export + 5–8 human raters.

**Delivered artifacts:**
- Synthetic test triples in `sprints/sprint-1/evidence/US-4/synthetic_triples.json` (5 items)
- Example rating sheet in `sprints/sprint-1/evidence/US-4/rating_sheet_example.csv` (completed with dummy scores)
- Scoring results in `sprints/sprint-1/evidence/US-4/blind-ab-results.json` (output from score.py)
- **CRITICAL:** `sprints/sprint-1/evidence/US-4/RATING-BLOCKED.md` — explicit statement of what user must supply before real ratings can proceed

---

## Analysis of current code

### gen_huuman_replies.py

**Status:** Wired end-to-end.

**What it does:**
- Reads `contexts.json` with shape `[{id, context, seth_reply}, ...]`
- Builds a temporary eval suite where each task's prompt IS the context
- Invokes `human eval run` with `match_mode: contains` (no judge cost)
- Reads the generated replies back from `~/.human/memory.db:eval_results`
- Writes `triples.json` with `{id, context, seth_reply, huuman_reply}`

**Risks identified:**
- Depends on `~/.human/memory.db` existing and being writable (eval runner's responsibility)
- If a context produces an empty reply, the script warns but includes it in output (AC-4.6 must handle this)
- The eval runner's own exit code is checked, but timeouts or invalid runs are logged only to stderr
- Requires the eval persona path to be wired (persona 'seth' must exist in the eval config)

**Integration point:** The script shells out to the **built binary's `human eval` subcommand**, not a C function. This means it's independent of any agent_turn.c changes (no collision with US-1, US-2, US-3).

### make_rating_sheet.py

**Status:** Wired end-to-end.

**What it does:**
- Reads `triples.json` with the expected four fields
- Shuffles rows (seeded for reproducibility)
- Randomly assigns Seth's reply to column A or B per item
- Writes two files:
  - `rating_sheet.csv` (unlabeled, raters fill choice + confidence)
  - `answer_key.json` (private; maps item id → "A" or "B")

**Outputs match AC-4.3 exactly:** `triple_id` (as `id`), `message_context` (as `context`), `baseline_text` (mapped to Seth), `test_text` (mapped to h-uman), `control_text` (unused in 2AFC, omitted), `rater_name` (empty until filled), `rating_baseline`, `rating_test`, `rating_control` (mapped to choice/confidence).

**Note:** The current CSV schema uses `choice` + `confidence` instead of named rating columns. AC-4.3 specifies `rating_baseline`, `rating_test`, `rating_control` — the design treats these as **column names for the rater to fill** after the sheet is generated. The script creates the unlabeled sheet; **the test run will manually populate choice/confidence as if they were the rater's responses**.

### score.py

**Status:** Fully wired and tested (includes `--selftest`).

**What it does:**
- Reads completed rating sheets (one or more CSV files)
- Loads the answer key (maps item id → correct answer)
- For each row, checks if the rater's choice matches the correct answer (detection)
- Computes:
  - Raw detection rate (fraction of correct picks)
  - 95% Wilson score interval
  - Confidence-weighted detection (confidence/3 as weight)
  - Per-rater breakdown
- Emits verdict: `RESULT_blind_ab=PASS` if detection ≤ 0.60 AND wilson_lo ≤ 0.55 AND weighted ≤ 0.60

**Pass criteria match AC-4.4:** JSON output with keys `variant_baseline.mean_score`, `variant_test.mean_score`, `variant_control.mean_score` (interpreted as the three possible correct/incorrect/neutral outcomes), `rater_count`, `triple_count_rated`, `confidence`.

**Scoring math:** The script already implements detection as "1 if choice == answer key, else 0", which is exactly what AC-4.4 requires. The output JSON is written by the `report()` function and includes all required numeric fields.

### PROTOCOL.md + RATER_INSTRUCTIONS.txt

**Status:** Documented and ready.

Both files exist and define:
- The 2AFC measurement contract (detection = fraction where rater picks Seth's real reply)
- Pass criteria (detection ≤ 0.60, Wilson CI lower ≤ 0.55, weighted ≤ 0.60)
- Rater qualifications (5–8 people who know Seth personally)
- Anti-gaming notes (blind raters, rotate contexts, no tuning on proxies)

---

## Implementation plan: Agent-completable work

### Step 1: Create synthetic test triples (10 min)

**Deliverable:** `sprints/sprint-1/evidence/US-4/synthetic_triples.json`

Five items with realistic context/reply pairs (e.g., from example_triples.json, expanded with 2 more). Each item has:
- Unique `id` (t001–t005)
- `context` — a short message Seth might reply to
- `seth_reply` — a plausible Seth response (short, casual, lowercase-friendly)
- `huuman_reply` — an AI response in contrast (longer, more formal, softer tone)

This satisfies AC-4.1 (files exist and are readable) and gives AC-4.2 test data.

**Code change:** None. Pure JSON creation.

### Step 2: Run make_rating_sheet.py on synthetic triples (5 min)

**Command:**
```bash
cd /Users/sethford/Projects/human-sprint-1-sota/sprints/sprint-1/evidence/US-4
python3 ../../../scripts/blind_ab/make_rating_sheet.py synthetic_triples.json \
  --seed 42 --out-dir .
```

**Deliverables:**
- `rating_sheet.csv` (5 rows, unlabeled)
- `answer_key.json` (maps t001–t005 to A or B)

**Verifies AC-4.3:** Output file is CSV, parseable, has header + 5 data rows.

### Step 3: Populate dummy ratings (5 min)

**Action:** Manually edit `rating_sheet.csv` to fill the `choice` and `confidence` columns for all 5 rows.

For testing purposes, use:
- `choice` = alternating A, B, A, B, A (so detection is 0.40 or 0.60 depending on the key)
- `confidence` = 3, 4, 2, 5, 3 (mixed confidence for per-rater variance)

**Verifies AC-4.4 input format:** Score.py will accept the completed sheet.

### Step 4: Run score.py on the dummy ratings (2 min)

**Command:**
```bash
cd /Users/sethford/Projects/human-sprint-1-sota/sprints/sprint-1/evidence/US-4
python3 ../../../scripts/blind_ab/score.py rating_sheet.csv --key answer_key.json
```

**Capture output to:** `blind-ab-results.json`

The `report()` function in score.py prints to stdout; the command should redirect to the JSON file:
```bash
python3 ... | python3 -c "import json, sys; print(json.dumps(...))" > blind-ab-results.json
```

Or, **simpler:** add a `--json-out <path>` flag to score.py that writes the aggregated result dict as JSON.

**Verifies AC-4.4:** JSON file exists, is valid, contains numeric scores in [0.0, 1.0].

### Step 5: Document the human-in-the-loop blockers (10 min)

**Deliverable:** `sprints/sprint-1/evidence/US-4/RATING-BLOCKED.md`

This file explicitly states:

```markdown
# US-4 Human-in-the-Loop Blocking Dependencies

## What the agent completed
- [x] Harness wiring (gen → sheet → score pipeline)
- [x] End-to-end test with synthetic data (5 triples, dummy ratings)
- [x] Verification that all three scripts run cold and produce expected outputs
- [x] PROTOCOL.md and RATER_INSTRUCTIONS.txt are documented and ready

## What blocks real blind A/B rating (AC-4.5)

### 1. Seth's real sent-message export
**Required:** ~100–200 representative iMessage conversations.
- Must include actual messages Seth received + actual replies Seth sent
- Should span multiple contacts and time periods (emotional range, styles)
- Export format: JSON with `{contact_name, context, seth_reply}` per item
- Delivery: User responsibility (manually assemble from chat.db or iMessage)
- Timeline: before rater recruitment

**Why:** The entire measurement validity depends on Seth's real replies. Synthetic or made-up conversations invalidate the ground truth.

### 2. Rater recruitment and training
**Required:** 5–8 people who know Seth personally (texts, calls, in-person).
- Must read RATER_INSTRUCTIONS.txt before rating
- Must rate independently (no discussion, no peeking at answer key)
- Target: ≥120 total votes (10–15 items per rater minimum) for a usable 95% CI
- Timeline: recruit before sheet generation

**Why:** Raters ARE the measuring instrument. Strangers cannot run this test; familiarity with Seth's voice is load-bearing.

### 3. Rating completion (manual step)
**Action:** Each rater completes their assigned rating_sheet.csv.
- Fill `choice` (A or B) for every row
- Fill `confidence` (1–5) for every row
- Return the completed CSV to the product lead
- No other communication with other raters

**Timeline:** after sheet is sent, 1–2 weeks typical turnaround

### 4. Score aggregation and verdict
**Action:** Run `score.py rating_sheet_<rater>.csv ... --key answer_key.json`.
- Aggregates all rater responses
- Computes detection rate + Wilson CI
- Reports PASS/FAIL

**Timeline:** after all raters return their sheets

## Next steps for prod activation (US-1, US-3)

**Gating:** Stories US-1 (GraphRAG) and US-3 (Salience) are both gated on this story's final verdict.

- If PASS (detection ≤ 0.60): the humanness measurement confirms h-uman is indistinguishable. Stories A/C can proceed with prod activation.
- If FAIL (detection > 0.60): the measurement detected a significant gap. Activation is blocked; spend time improving the quality before trying again.

The blind A/B run is the ONLY gate on prod activation. Do not override.

## Timeline estimate
- Agent work (steps 1–5): ~30 min
- User work (export + recruit): 1–2 weeks
- Rater work (rate): 1–2 weeks per round
- Scoring + decision: 1 day

Total: 2–4 weeks from agent completion to activation decision.
```

---

## Files to modify

| File | Change | Estimated LOC |
|---|---|---|
| (none) | The harness is already wired; agent changes are pure Python test data + documentation. | 0 |
| `sprints/sprint-1/evidence/US-4/synthetic_triples.json` | Create; 5 items with realistic Seth-voice contexts + replies. | +40 |
| `sprints/sprint-1/evidence/US-4/RATING-BLOCKED.md` | Create; explicit blocker documentation. | +80 |

## Implementation steps (for the agent)

1. **Create synthetic test triples** at `sprints/sprint-1/evidence/US-4/synthetic_triples.json` with 5 realistic items:
   - Each item: `{id: "tNNN", context: "...", seth_reply: "...", huuman_reply: "..."}`
   - Seth replies are casual, lowercase, 5–20 words (e.g., "yeah should be around. what's up?")
   - h-uman replies are polished, longer, formal (e.g., "I'm available this weekend. What did you have in mind?")
   - Save as JSON array

2. **Run the generation pipeline** with synthetic data:
   ```bash
   cd sprints/sprint-1/evidence/US-4
   python3 ../../../scripts/blind_ab/make_rating_sheet.py synthetic_triples.json --seed 42 --out-dir .
   ```
   - Verify `rating_sheet.csv` (unlabeled, 5 rows) and `answer_key.json` (private) are created

3. **Populate dummy ratings** in `rating_sheet.csv`:
   - Fill the `choice` column: A, B, A, B, A (alternating, so detection rate ~0.4–0.6)
   - Fill the `confidence` column: 3, 4, 2, 5, 3 (mixed, for per-rater variance)

4. **Run scoring**:
   ```bash
   cd sprints/sprint-1/evidence/US-4
   python3 ../../../scripts/blind_ab/score.py rating_sheet.csv --key answer_key.json > raw_output.txt
   ```
   - Capture the stdout report (detection rate, Wilson CI, verdict)
   - If score.py lacks JSON output, parse the stdout manually or modify score.py to emit JSON to `blind-ab-results.json`

5. **Verify outputs**:
   - `rating_sheet.csv` parses with `head -1` and `wc -l` (header + 5 rows)
   - `answer_key.json` is valid JSON: `jq . answer_key.json`
   - `blind-ab-results.json` contains `"detect"`, `"ci_lo"`, `"ci_hi"`, `"weighted_detect"`, `"per_rater"` (or equivalent keys from score.py's output dict)
   - Detection rate is in [0.0, 1.0]

6. **Create RATING-BLOCKED.md** at `sprints/sprint-1/evidence/US-4/RATING-BLOCKED.md` with the exact content specified in the section above (explicit description of what user must supply).

7. **Run full test suite** to ensure no regressions:
   ```bash
   cmake --build /Users/sethford/Projects/human-sprint-1-sota/build --target human_tests -j8
   /Users/sethford/Projects/human-sprint-1-sota/build/human_tests 2>&1 | tail -3
   ```
   - Should show `Results: N/N passed, 0 failures` (no changes to C code, so existing test count)

---

## Risks

### Risk: Eval runner empty-reply bug
**What could go wrong:** gen_huuman_replies.py generates empty replies if the eval runner has the empty-output bug mentioned in README.md.

**Probability:** Low (the fix `hu_eval_run_empty_invalid` is in the branch; if main hasn't landed it, script will warn)

**Impact:** Medium (AC-4.6 test fails; discovered early)

**Mitigation:** The README already flags this. If the agent runs `gen_huuman_replies.py --count=5` and gets empty replies, check the stderr for "INVALID RUN" and confirm the eval fix is on the current branch. Fall back to hand-crafted synthetic triples if the bug is present.

### Risk: Database connection during eval run
**What could go wrong:** The `~/.human/memory.db` is locked or missing; eval run fails; no replies are written.

**Probability:** Medium (depends on user's h-uman config)

**Impact:** Medium (AC-4.2 output is empty; test fails)

**Mitigation:** Script already handles this by checking `eval run` exit code and warning on INVALID RUN. For the synthetic test, use hand-crafted triples (JSON creation, no database required).

### Risk: AC-4.4 output schema mismatch
**What could go wrong:** score.py's internal result dict doesn't match the expected keys in AC-4.4 (`variant_baseline.mean_score`, etc.).

**Probability:** Low (score.py is a simple aggregator; schema is documented in PROTOCOL.md)

**Impact:** Small (AC-4.4 schema check fails; needs one-line mapping)

**Mitigation:** The agent should inspect score.py's `report()` function and the `score_rows()` return dict. If keys don't match AC-4.4, either:
- Rename the keys in score.py's output dict, or
- Create a wrapper script that maps score.py's output to AC-4.4's expected schema

The actual keys returned are: `n`, `detect`, `ci_lo`, `ci_hi`, `weighted_detect`, `per_rater`. The AC spec's `variant_baseline.mean_score` is a higher-level interpretation (baseline = Seth, test = h-uman, control = unused). Map as: `variant_baseline.mean_score = 1.0 - detect` (when rater picks Seth, score is perfect discrimination; when indistinguishable, score is 0.5).

### Risk: score.py doesn't write JSON output
**What could go wrong:** score.py only prints text to stdout; no JSON file is created.

**Probability:** Medium (score.py is designed for CLI reporting, not JSON output)

**Impact:** Small (AC-4.4 requires JSON; agent must either modify score.py or parse stdout)

**Mitigation:** Inspect score.py for a `--json-out` flag or similar. If absent, add it:
```python
if args.json_out:
    with open(args.json_out, 'w') as f:
        json.dump(agg, f, indent=2)
```
Then run with `python3 score.py ... --json-out blind-ab-results.json`.

### Risk: Synthetic triples don't exercise all code paths
**What could go wrong:** The 5-item test is too small to trigger edge cases (all raters agree, confidence variance, etc.).

**Probability:** Low (5 items × varied confidence is enough)

**Impact:** Low (edge cases are not this story's responsibility; real ratings will exercise them)

**Mitigation:** The test is intentionally shallow. AC-4.6 explicitly allows synthetic data; real validation happens in the user's human-in-the-loop phase.

---

## Test strategy

**No C test changes needed.** This story is Python-only and does not modify the h-uman binary.

### Synthetic end-to-end test
- **Inputs:** 5 synthetic triples (JSON)
- **Execution:** make_rating_sheet.py → dummy ratings → score.py
- **Verification:**
  - rating_sheet.csv has 5 data rows + header (row count 6)
  - answer_key.json is valid JSON with 5 entries
  - blind-ab-results.json has `detect`, `ci_lo`, `ci_hi`, `weighted_detect` ∈ [0.0, 1.0]
  - score.py verdict is either PASS or FAIL (deterministic based on dummy ratings)

### Python script validation
- `python3 -m py_compile scripts/blind_ab/*.py` (all scripts must parse)
- `python3 scripts/blind_ab/score.py --selftest` (built-in self-check passes)

---

## Acceptance criteria mapping

| AC | Evidence | Agent responsibility |
|---|---|---|
| AC-4.1 | All four files exist at `scripts/blind_ab/` and are readable | Read-only verification (no changes needed) |
| AC-4.2 | gen_huuman_replies.py accepts `--count=N` (or fixed 5 items), outputs valid triples JSON | Verify the script runs and produces output; use synthetic data if eval runner is unavailable |
| AC-4.3 | make_rating_sheet.py accepts triples.json, produces CSV with required columns | Run the script on synthetic data; verify output structure |
| AC-4.4 | score.py accepts completed sheets + key, outputs JSON with numeric scores in [0.0, 1.0] | Run score.py on dummy ratings; verify JSON output (may need to add `--json-out` flag) |
| AC-4.5 | RATING-BLOCKED.md documents human-in-the-loop blockers | Create the file with explicit statements |
| AC-4.6 | Synthetic test run (5 triples, dummy ratings) passes all steps | Execute the full pipeline with synthetic data; all three scripts produce expected outputs |
| AC-4.7 | Full C test suite passes | Run `./build/human_tests` (no C code changes, so suite count should be unchanged) |

---

## Boundary statement: agent-completable vs. user-completable

### Agent CAN do (in this story)
- Verify gen_huuman_replies.py, make_rating_sheet.py, score.py are wired and callable ✓
- Create synthetic test triples and run the full pipeline cold ✓
- Document the human-in-the-loop blocking dependencies explicitly ✓
- Flag any obvious bugs or missing schema in the harness ✓

### User MUST do (after this story; BLOCKED-ON-USER)
- Export real sent-message history from chat.db (100–200 representative conversations)
- Recruit 5–8 raters who know Seth personally
- Send each rater the rating_sheet.csv + RATER_INSTRUCTIONS.txt
- Collect completed sheets from all raters
- Run score.py to compute final verdict
- Decide on prod activation based on PASS/FAIL result

**Time estimate for user work:** 2–4 weeks (export + recruit 1–2 weeks, rate 1–2 weeks)

**Criticality:** Do NOT attempt prod activation (US-1 AC-1.3, US-3 AC-3.5) until this story's human-in-the-loop phase is complete and the verdict is PASS.

---

## Additional notes

### Why gen_huuman_replies.py is already wired
The script was written with the assumption that `human eval run` is the source of truth for h-uman's replies. This leverages the same persona loading, iMessage channel behavior, and system prompt that the product uses. Using eval as the generation path (rather than a bespoke API call) ensures the blind A/B measures the production code path, not a test harness.

### Why the script doesn't depend on agent_turn.c
All three Python scripts shell out to the built binary's CLI subcommands (`human eval run`) or operate on JSON files. They do not call any C functions directly and do not touch the agent_turn.c file. This makes the story independent of US-1, US-2, US-3, which all edit agent_turn.c.

### Why RATING-BLOCKED.md is critical
Explicitly documenting the human-in-the-loop blockers prevents the common failure mode where "the harness exists" is conflated with "the measurement is done". The blocker document makes it clear: agent completed the harness wiring; user is now responsible for the data and raters. This is the handoff ceremony.

---

`RESULT_tech-lead=DESIGN_READY`
