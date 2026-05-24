# Rounds 5-10 Execution Plan — 34 hours, every task proven empirically

> Companion to `2026-05-19-vision-better-than-human.md`. That doc said
> WHAT the 6 rounds deliver. This doc says HOW we'll know each piece
> works.

## Methodology — four levels of proof per task

| Level | What it answers | When required |
|---|---|---|
| **L1 build** | Does the code compile clean with `-Werror`? | Every task |
| **L2 unit** | Does the function match its contract in isolation? | Every task that adds a function |
| **L3 integration** | Does it work when wired into a real pipeline? | Every task that touches the daemon or eval suite |
| **L4 production** | Does real production traffic prove the metric moved? | Every round closure |

Tasks that can't reach L4 in a session document the **L4 success metric**
that operators will watch for in the days after merge.

Pattern for every task:
1. **Design** — what the function/wire does, why it lives where it does
2. **Test FIRST** — write the unit + integration tests as failing assertions
3. **Build** — minimum code to flip the tests green
4. **Verify** — run the full suite to confirm no regression
5. **Document** — what production signal proves it works

## Round 5 — finishing AGI Capability-1 (remaining ~5hr)

P5b is done. The remaining gaps: latency ingest, alternatives column,
p_seth_at_send actual value.

### R5.1 — `hu_dpo_record_outcome` accepts reply latency from message_inbound (1hr)

**Goal**: when an inbound iMessage arrives, look up the most-recent
outbound to that contact, compute the latency, call `record_outcome`
with `reply_latency_s` set. This is the universal signal — tapbacks
are rare, replies are not.

**Where**: `src/channels/imessage.c::imessage_inbound_msg` (or wherever
the inbound dispatch lives). Find the point where a `message_inbound`
event is delivered to the daemon.

**Tests (write first)**:
- `tests/test_dpo.c::dpo_record_outcome_with_latency_sets_column` — record
  outbound at t=0, call record_outcome with reply_latency_s=42 at any
  later time, assert the row's reply_latency_s column = 42.
- `tests/test_imessage_inbound.c::inbound_after_outbound_records_latency`
  — full integration: write outbound at t=0, simulate inbound at t=120s,
  assert production_outcomes row has reply_latency_s ≈ 120.

**Verify**:
```
./build/human_tests --filter=record_outcome_with_latency
./build/human_tests --filter=inbound_after_outbound
./build/human_tests | grep "Results:"  # expect 11584/11584
```

**Production signal**: after 24h of real traffic, query
`SELECT COUNT(*) FROM production_outcomes WHERE reply_latency_s IS NOT NULL`
should be ≥ the count of inbound messages from outbound-target contacts.

### R5.2 — store best-of-N alternatives in `alternatives` column (1hr)

**Goal**: when production runs best-of-N (Round 6 will add this), the
losers go into `alternatives` as a JSON array. The DPO pair generator
in `outcomes_to_dpo.py` already reads this column; today it's NULL
because we don't run best-of-N in production yet.

**Where**: extend `hu_dpo_record_outbound` to accept an optional
`alternatives_json` string parameter. The agent_turn caller in daemon
passes the JSON when L5 fires; passes NULL otherwise.

**Tests**:
- `dpo_record_outbound_with_alternatives_persists_json` — write a row with
  alts=`["yeah", "for sure", "I'll be there"]`, read back, assert json
  parses to 3-element array.
- `dpo_record_outbound_null_alternatives_stores_null` — write without alts,
  assert column IS NULL.

**Verify**:
```
./build/human_tests --filter=alternatives
sqlite3 /tmp/agi_smoke.db "SELECT alternatives FROM production_outcomes;"
```

**Production signal**: when Round 6 lands and best-of-N fires, ≥80% of
production_outcomes rows have non-NULL alternatives.

### R5.3 — agent_turn computes p_seth_at_send via the C classifier (2hr)

**Goal**: today `record_outbound` is called with `-1.0` for p_seth.
After this task, the daemon loads the C classifier once at startup,
scores the response right before send, passes the real value.

**Where**:
- `src/agent.c` (agent init) — load model with `hu_persona_eval_load`,
  store pointer in `hu_agent_t`.
- `src/daemon.c` (post-agent-turn site) — call `hu_persona_eval_score`
  on `response`, pass to `record_outbound`.

**Design subtlety**: the classifier file may not exist on first boot.
Loader must return HU_OK with model=NULL gracefully; downstream code
must handle NULL by passing -1.0 (existing behavior).

**Tests**:
- `tests/test_agent.c::agent_init_with_persona_eval_model_present_loads_it`
- `tests/test_agent.c::agent_init_with_missing_model_proceeds_without_failure`
- `tests/test_dpo.c::record_outbound_with_p_seth_persists_column` — verify
  the value flows through to the SQLite row.

**Verify**:
```
./build/human_tests --filter=persona_eval
./build/human_tests | grep "Results:"
# Restart daemon, send one test prompt through gateway:
curl -X POST http://127.0.0.1:3006/v1/chat/completions \
  -d '{"messages":[{"role":"user","content":"hey"}]}'
sqlite3 ~/.human/memory.db "SELECT p_seth_at_send FROM production_outcomes ORDER BY id DESC LIMIT 1;"
# Expect: a non-NULL value in [0, 1], NOT -1.0
```

**Production signal**: ≥95% of production_outcomes rows have non-NULL
p_seth_at_send within 24h.

### R5.4 — Round 5 integration test + closure (1hr)

**Goal**: prove the full loop fires end-to-end on real-ish traffic.

**Approach**: write `tests/test_agi_c1_e2e.c` that exercises the whole
chain with mocks:
1. Mock provider returns a fixed response
2. agent_turn produces the response
3. Daemon writes production_outcomes row with non-NULL p_seth_at_send
4. Mock inbound arrives 60s later (use fake clock)
5. record_outcome fires with reply_latency_s=60
6. Resolved row processed by outcomes_to_dpo (Python harness)
7. Resulting dpo_pairs row has source='outcome_fast_reply', margin=0.40

**Verify**: a single bash command runs the full chain and confirms 1
dpo_pair output.

**Round 5 closure metric (production)**: after 7 days of real iMessage
traffic, the launchd-scheduled outcomes_to_dpo run produces ≥10
dpo_pairs rows of source `outcome_*`. That's the empirical "the loop
is producing real training data" signal.

---

## Round 6 — meta-cognitive uncertainty (3hr)

The capability that turns h-uman from "always sends" into "knows when
to defer."

**Design decision (user input recommended)**: the threshold values.
Conservative (defer often, fewer mistakes): defer < 0.6, best-of-N <
0.85. Aggressive (defer rarely, more agency): defer < 0.4, best-of-N
< 0.7. The proposed midpoint is defer < 0.5, best-of-N < 0.8.

### R6.1 — uncertainty router in agent_turn (1hr)

**Where**: `src/agent/agent_turn.c` after the response is finalized but
before send.

**Logic**:
```c
double p_seth = hu_persona_eval_score(agent->persona_eval_model,
                                       response, response_len);
if (p_seth >= 0.80) {
    /* ship single-shot, existing path */
} else if (p_seth >= 0.50) {
    /* fire best-of-N (n=5), pick filtered argmax P(Seth) */
    /* run_best_of_n returns the highest-P(Seth) candidate */
} else {
    /* defer: respond with a clarifying question OR no response,
     * log to deferred_turns for review */
}
```

**Tests**:
- `agent_turn_high_p_seth_ships_single` — pass classifier returning 0.85,
  assert only one model call was made.
- `agent_turn_medium_p_seth_fires_best_of_n` — classifier 0.65, assert N
  model calls and final response is the highest-P(Seth) candidate.
- `agent_turn_low_p_seth_defers` — classifier 0.30, assert response is
  empty AND a row was added to deferred_turns table.

### R6.2 — `deferred_turns` table + observability (1hr)

**Schema**:
```sql
CREATE TABLE deferred_turns(
  id INTEGER PRIMARY KEY,
  channel TEXT, target TEXT,
  prompt TEXT, candidate TEXT,
  candidate_p_seth REAL,
  alternatives TEXT,  -- JSON of all N candidates with scores
  deferred_at INTEGER,
  resolved_action TEXT  -- 'user_drafted' | 'sent_anyway' | 'dropped'
);
```

**Tests**:
- `deferred_turn_recorded_on_low_confidence` — verify the row is written
  with all columns populated, alternatives JSON parses.

### R6.3 — best-of-N runner using the C classifier (1hr)

**Where**: new `src/agent/best_of_n_runner.c` (or extend existing
best_of_n.c).

**Logic**: call provider->chat N times with varying temperature, score
each, return filtered argmax.

**Tests**:
- `best_of_n_filters_and_argmax` — mock provider returns 5 responses with
  varying P(Seth); verify the highest-passing one wins.
- `best_of_n_all_fail_shape_falls_back` — all 5 fail shape; verify the
  fallback chooses least-bad by shape score.

**Round 6 closure**: integration test that exercises all three
branches (ship / best-of-N / defer) and verifies each writes the
correct row.

**Round 6 metric (production)**: after 7 days, the distribution of
single-shot / best-of-N / deferred should be roughly 60/30/10. Any
distribution where deferred > 25% or best-of-N < 5% indicates
threshold miscalibration.

---

## Round 7 — per-conversation summary → personal_model (6hr)

The capability where h-uman literally learns about people.

### R7.1 — conversation lull detection (1hr)

**Goal**: detect when a conversation has ended (no message in ≥6h
from either party).

**Where**: extend the existing `hu_conversation_*` API in
`src/conversation/`.

**Tests**:
- `lull_detected_after_6h_silence`
- `no_lull_within_active_window`
- `multiple_lulls_per_thread_each_triggered_once`

### R7.2 — summarization LLM call (1.5hr)

**Goal**: given the messages in a lulled conversation, generate a list
of typed propositional facts.

**Where**: new `src/agent/conv_summary.c`.

**API**:
```c
hu_error_t hu_conv_summary_extract(
  hu_agent_t *agent,
  const hu_chat_message_t *messages, size_t n_messages,
  hu_fact_t **out_facts, size_t *out_n);
```

The function builds a system prompt explicitly asking for typed
triples (subject/predicate/object/confidence), then parses the JSON
response.

**Tests**:
- `summary_extracts_named_entity_facts` — input a 5-turn conversation
  about "Casey moving to Austin," assert at least one fact has
  subject="Casey", object includes "Austin".
- `summary_handles_empty_thread` — 0 messages → 0 facts, no crash.
- `summary_handles_malformed_llm_response` — mock returns garbage,
  assert HU_ERR_PARSE not crash.

### R7.3 — fact ingestion into personal_model (1.5hr)

**Goal**: facts from summary go into `personal_model.facts` via the
existing `hu_personal_model_ingest_fact` API. Deduplicate against
existing facts.

**Tests**:
- `fact_ingest_appends_new_fact`
- `fact_ingest_deduplicates_identical_subj_pred_obj`
- `fact_ingest_updates_confidence_on_dup_with_higher_confidence`

### R7.4 — scheduler that triggers summarization on lull (1hr)

**Where**: `src/daemon.c` scheduler tick.

**Logic**: scan conversations, find newly-lulled ones, queue
summarization jobs. Don't run summaries inline (would block the
turn).

**Tests**:
- `lulled_conversation_queued_once`
- `summarization_job_completes_async`

### R7.5 — Round 7 integration + closure (1hr)

**Integration test**: simulate a 10-turn conversation, advance the
clock past the 6h lull threshold, verify:
- summarization fires
- at least 1 fact lands in personal_model
- the fact appears in the next turn's system prompt to the same
  contact

**Round 7 metric (production)**: after 14 days, `personal_model.facts`
table has ≥30 new facts NOT in the initial example_banks.

---

## Round 8 — commitment-driven proactive follow-up (4hr)

### R8.1 — extend commitment_store with `proactive_due_at` (0.5hr)

```sql
ALTER TABLE commitments ADD COLUMN proactive_due_at INTEGER;
```

**Tests**:
- `commitment_schema_includes_proactive_due_at`
- `commitment_default_proactive_due_at_is_due_minus_24h`

### R8.2 — scheduler scan for due commitments (1hr)

**Where**: `src/daemon_cron.c`.

**Logic**: every N minutes, SELECT commitments WHERE proactive_due_at <
now AND status='open'. For each: queue a follow-up draft.

**Tests**:
- `scheduler_finds_due_commitments`
- `scheduler_skips_already_resolved_commitments`

### R8.3 — "did this resolve?" check before sending follow-up (1hr)

**Goal**: don't follow up if the conversation already covered it.
Search recent messages for keywords from the commitment.

**Tests**:
- `followup_skipped_if_commitment_text_appears_in_recent_messages`
- `followup_fires_if_no_recent_match`

### R8.4 — Round 8 integration + closure (1.5hr)

**Integration test**: create a commitment with due_at 24h from now,
advance clock past proactive_due_at, verify a follow-up draft is
queued, AND verify the draft references the commitment's topic.

**Round 8 metric (production)**: ≥1 proactive follow-up sent per
week, with operator-confirmed "good timing" rate ≥70% (subjective —
ask operator weekly).

---

## Round 9 — reflexion on negative outcomes (6hr)

### R9.1 — `production_lessons` table (0.5hr)

```sql
CREATE TABLE production_lessons(
  id INTEGER PRIMARY KEY,
  context_pattern TEXT,  -- short tag like "ai-tell-reply-to-vent"
  rule_text TEXT,        -- "don't say 'I understand how that feels'"
  source_outcome_id INTEGER,  -- which production_outcomes row triggered this
  created_at INTEGER,
  times_triggered INTEGER DEFAULT 0,
  reverted INTEGER DEFAULT 0  -- 1 if operator/critic rejected the rule
);
```

### R9.2 — reflexion trigger on negative outcome (1hr)

**Where**: in `outcomes_to_dpo.py` OR a new `reflexion_runner.py`.

**Logic**: when an outcome resolves with `tapback_polarity=-1` OR
`reply_latency_s > 21600`, queue a reflexion LLM call.

**Tests**: Python-side unit test for the trigger logic.

### R9.3 — reflexion LLM call + rule extraction (2hr)

**Prompt template**:
```
You sent '[chosen]' as the response to '[prompt]'. It got a negative
tapback (or no reply in 6h). What specifically about that response was
wrong? Output a JSON object: {context_pattern: "...", rule: "...",
confidence: 0.0-1.0}.
```

**Parser** validates the response JSON, requires confidence > 0.7 to
write the rule.

**Tests**:
- `reflexion_extracts_rule_from_failure`
- `reflexion_skips_low_confidence`

### R9.4 — persona prompt builder includes relevant lessons (1.5hr)

**Where**: `src/agent/prompt.c`.

**Logic**: when building the system prompt, query `production_lessons`
for rules whose `context_pattern` matches the incoming. Include in the
prompt under a "lessons learned from past mistakes" section.

**Tests**:
- `prompt_includes_relevant_lesson`
- `prompt_omits_lessons_for_unrelated_context`
- `prompt_omits_reverted_lessons`

### R9.5 — Round 9 closure (1hr)

**Integration test**: simulate a negative outcome, verify a lesson
lands in the table, verify subsequent prompt to the same context
includes the rule.

**Round 9 metric (production)**: after 4 weeks, `production_lessons`
has ≥20 rules. Sample of 10 rules: operator confirms ≥70% are real
lessons (not LLM-confabulated noise).

---

## Round 10 — multi-turn proactive (8hr)

This is the hardest because it requires TASTE — the line between
"helpful friend" and "annoying assistant" is delicate.

### R10.1 — conversation state classifier: open / closed / paused (1.5hr)

**Goal**: distinguish "this conversation is done" from "this
conversation will likely continue." Different proactive triggers
apply.

**Heuristics**:
- Last message was a question from us → paused (waiting for reply)
- Last message ended with "see you" / "talk later" / "ttyl" → closed
- 24h+ no activity but no terminator → closed (probably)
- Open commitment present → check at proactive_due_at regardless

**Tests**:
- `conversation_state_detects_closed_after_terminator`
- `conversation_state_detects_paused_on_our_question`
- `conversation_state_detects_open_during_active_back_and_forth`

### R10.2 — contact activity window inference (1.5hr)

**Goal**: don't text a contact at 3am if they never reply at 3am. Use
historical message timestamps to infer when they're typically active.

**Tests**:
- `activity_window_inferred_from_recent_inbound_timestamps`
- `activity_window_handles_sparse_history`

### R10.3 — multi-turn proactive scheduler (2hr)

**Where**: extend `src/daemon_cron.c`.

**Logic** combines:
- Commitments from R8 (already scheduled)
- Personal_model facts that imply follow-up ("Casey has a job interview
  Friday" → text Casey Friday evening "how'd it go?")
- Activity-window timing

**Tests**:
- `proactive_skipped_outside_activity_window`
- `proactive_queued_for_due_commitment`
- `proactive_queued_for_fact_anchored_event`

### R10.4 — proactive draft generation with context anchor (1.5hr)

**Where**: extend agent_turn or add `src/agent/proactive_drafter.c`.

**Logic**: build a prompt that explicitly references the anchor
("Casey's job interview" / "the doctor's appointment Pat mentioned").
The draft must reference the anchor or it gets rejected.

**Tests**:
- `proactive_draft_references_anchor`
- `proactive_draft_rejected_if_no_anchor_reference`

### R10.5 — Round 10 closure (1.5hr)

**Integration test**: set up an open commitment + a personal_model
fact ("Pat is going to a job interview on May 23"). Advance clock to
proactive_due_at. Verify a draft is queued. Verify draft references
the fact. Verify draft is timed to Pat's inferred activity window.

**Round 10 metric (production)**: after 4 weeks of operation,
positive tapback rate on proactive messages ≥ positive tapback rate
on reactive messages. (If the proactive ones land worse than the
reactive ones, the system is being annoying.)

---

## Cross-round integration: the "real friend" test

After all 6 rounds land, run this integration test in a sandboxed
production-like environment:

1. Day 1: incoming "hey, going through some stuff with my mom"
   - Expected: empathetic short reply (Round 6 may best-of-N for
     this sensitive case)
   - personal_model gains fact: contact's mom is unwell (R7)

2. Day 2: incoming "thanks for listening yesterday"
   - Expected: tapback love (L4 wired) OR short text
   - personal_model.facts referenced in prompt

3. Day 5 (proactive): R10 should queue a "hey how's your mom doing"
   - draft must reference the fact from Day 1
   - draft timed to contact's activity window

4. Day 5 outbound: P(Seth) ≥ 0.8 (Round 6) so ships single-shot

5. Inbound reply at Day 5 + 30 min:
   - record_outcome with reply_latency_s=1800
   - outcomes_to_dpo generates a fast-reply DPO pair
   - row added to dpo_pairs

6. Day 6: nightly ORPO retrains on the new pair
7. Day 7: new outbound is measurably shaped by Day 5's outcome

This is the FULL LOOP — perception (R7), reasoning (R9), expression
(R6), follow-through (R8 + R10), learning (R5). Each round must
contribute its piece for the day-7 outcome to manifest.

## Time accounting

| Round | Effort | Cumulative |
|---|---:|---:|
| R5 (remaining) | 5 hr | 5 hr |
| R6 | 3 hr | 8 hr |
| R7 | 6 hr | 14 hr |
| R8 | 4 hr | 18 hr |
| R9 | 6 hr | 24 hr |
| R10 | 8 hr | 32 hr |
| Integration + closure | 2 hr | 34 hr |

Best done in 5 sessions of 6-7 hours each. Splitting by round
boundaries:
- Session 1: R5 finish + R6 (8 hr)
- Session 2: R7 (6 hr)
- Session 3: R8 + start R9 (8 hr)
- Session 4: finish R9 + R10 (8 hr)
- Session 5: integration + 1 week of real production observation (4 hr)

## What proves "done" at the end of Round 10

**Quantitative production metrics** (all must hold for 2 consecutive
weeks):
- ≥95% of outbound messages have non-NULL p_seth_at_send
- ≥80% of best-of-N runs fire `p_seth_argmax_filtered` mode
- production_outcomes table grows ≥100 rows/week with outcomes
  resolved
- outcomes_to_dpo produces ≥20 dpo_pairs/week from real outcomes
- production_lessons has ≥20 distinct rules
- personal_model.facts has ≥30 facts from real conversations
- ≥1 proactive follow-up per week is operator-confirmed "good"
- positive tapback rate on proactive ≥ positive on reactive

**Qualitative test** (the "real friend" test above runs successfully
end-to-end).

**Subjective bar** (the actual goal): in a blind A/B between human-Seth-
on-an-average-day and h-uman responses to the same incoming, h-uman
is preferred ≥50% of the time by an independent rater. That's the
"better than the average day" claim.

## Risks + mitigations

| Risk | Why | Mitigation |
|---|---|---|
| Round 6 over-defers, h-uman becomes silent | thresholds too conservative | Start at the midpoint (0.5/0.8); calibrate from production data after week 1 |
| Round 7 facts pollute the personal_model with hallucinations | LLM summarization is noisy | confidence threshold ≥0.7 on facts; weekly operator review for the first month |
| Round 8 proactive feels intrusive | bad timing or wrong commitments | start with explicit operator opt-in per-contact; expand only after positive feedback |
| Round 9 lessons become a contradictory list | LLM-generated rules conflict | enforce a max of 20 active rules; rotate out rules with low times_triggered after 30 days |
| Round 10 proactive volume too high | over-eager scheduler | hard rate limit: ≤1 proactive per contact per week, ≤3 proactive per day total |

## How a future session executes this

Each session opens with:
1. `git log --oneline -5` to confirm last commit
2. Read the relevant Round section
3. Pick the next un-done task
4. Write the test FIRST (failing assertion)
5. Build the minimum code to flip green
6. `./build/human_tests | grep "Results:"` — full suite must pass
7. Mark task done in this doc, commit with the round/task ID in the
   subject line
8. Move to next task

The plan doc IS the project tracker. No external Linear or JIRA needed.
