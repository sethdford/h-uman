---
title: "Sprint 46 — Round 5 finish: AGI Capability-1 high-fidelity loop"
created: 2026-05-19
sprint: 46
branch: sprint-46-r5-finish
spec: docs/plans/2026-05-19-rounds-5-10-execution.md
program: docs/plans/2026-05-19-agi-path.md
status: in-progress
---

# Sprint 46 — Round 5 finish

## Goal

Close the remaining gaps in AGI Capability-1 so production_outcomes
rows have high-fidelity data. Specifically:

- Inbound replies record `reply_latency_s` (today only tapbacks do)
- Best-of-N alternatives flow into the `alternatives` column (today NULL)
- `p_seth_at_send` holds the real classifier score (today the -1.0 placeholder)

After this sprint, the L5 best-of-N → DPO pipeline (Sprint 47) is unblocked,
and the daily `outcomes_to_dpo.py` job produces complete DPO pairs from
real production traffic.

## Stories

### Story R5.1 — record reply latency on inbound iMessage

**As** the production learning loop,
**I want** every inbound iMessage that follows an outbound to record the
            reply latency in `production_outcomes.reply_latency_s`,
**So that** `outcomes_to_dpo.py` can generate `outcome_fast_reply` and
            `outcome_slow_reply` DPO pairs from universal signal (not
            just rare tapbacks).

**Acceptance criteria:**

1. New API: `hu_dpo_record_outcome` accepts `reply_latency_s` parameter (already there per AGI-C1).
2. Daemon-side wire: on inbound iMessage, look up the most-recent outbound to
   that contact in `production_outcomes`, compute latency (now − send_timestamp),
   call `hu_dpo_record_outcome` with the value.
3. Lookup falls back to (channel, target) when message_ref is unavailable
   (already implemented in record_outcome).
4. Unit test: `dpo_record_outcome_with_latency_sets_column` — write outbound
   at known timestamp, call record_outcome with latency=42, assert row's
   `reply_latency_s` column equals 42.
5. Integration test: `inbound_after_outbound_records_latency` — write
   outbound at t=0, simulate inbound at t=120s (use injection point), assert
   matching row has `reply_latency_s` ≈ 120 (±5 for clock jitter).
6. Full suite passes (≥11582 + 2 new tests).

**Files expected to change:**

- `src/daemon.c` (or `src/channels/imessage.c`) — inbound handler
- `tests/test_dpo.c` — unit test
- `tests/test_imessage_inbound.c` — integration test (may need to create)

**Verifier evidence:**
```
./build/human_tests --filter=dpo_record_outcome_with_latency 2>&1 | grep -E "PASS|FAIL"
./build/human_tests --filter=inbound_after_outbound_records_latency 2>&1 | grep -E "PASS|FAIL"
./build/human_tests 2>&1 | grep "Results:" | tail -1
```

---

### Story R5.2 — store best-of-N alternatives in `alternatives` column

**As** the production learning loop,
**I want** `hu_dpo_record_outbound` to accept an `alternatives_json` parameter,
**So that** when Sprint 47 wires L5 best-of-N in production, the losers
            are persisted for `outcomes_to_dpo.py` to materialize as
            DPO pairs.

**Acceptance criteria:**

1. `hu_dpo_record_outbound` signature extended with `(const char *alternatives_json, size_t alternatives_json_len)`.
2. Header doc explains: NULL/0 length → column stays NULL; non-NULL → stored as TEXT for Python JSON parsing.
3. All existing callers (daemon.c) pass `NULL, 0` — no behavior change today.
4. Unit test: `dpo_record_outbound_with_alternatives_persists_json` — write a row with `[\"yeah\",\"sure thing\",\"absolutely\"]`, read back via SQL, assert the JSON parses to a 3-element array.
5. Unit test: `dpo_record_outbound_null_alternatives_stores_null` — write without alts, assert column IS NULL.
6. Full suite passes.

**Files:**
- `include/human/ml/dpo.h` — signature update + doc
- `src/ml/dpo.c` — implementation
- `src/daemon.c` — update the single existing caller to pass NULL
- `tests/test_dpo.c` — 2 new tests

**Verifier evidence:**
```
./build/human_tests --filter=alternatives 2>&1 | grep -E "PASS|FAIL"
./build/human_tests 2>&1 | grep "Results:"
grep "hu_dpo_record_outbound" include/human/ml/dpo.h | head -3
```

---

### Story R5.3 — agent_turn computes real p_seth_at_send

**As** the production learning loop,
**I want** the daemon to load the C-side PersonaEval classifier at startup
            and score every outbound response, passing the real value to
            `hu_dpo_record_outbound`,
**So that** `production_outcomes.p_seth_at_send` holds a real number
            (not the -1.0 placeholder), enabling downstream quality
            filtering AND Round 6's uncertainty router.

**Acceptance criteria:**

1. `hu_agent_t` gains a `hu_persona_eval_model_t *persona_eval` field (heap-owned).
2. `hu_agent_init` loads the model from `/tmp/seth_speaker_id.json` (or the
   configured path) via `hu_persona_eval_load`. Failure is non-fatal — the
   field stays NULL and downstream calls return the neutral 0.5.
3. `hu_agent_deinit` frees the model.
4. Daemon-side wire: at the existing `record_outbound` call site, replace the
   hardcoded `-1.0` with `hu_persona_eval_score(agent->persona_eval, response,
   response_len)`.
5. Unit test: `agent_init_with_persona_eval_model_present_loads_it` — touch
   the model file, init agent, assert field is non-NULL.
6. Unit test: `agent_init_with_missing_model_proceeds_without_failure` —
   point at nonexistent path, assert init returns HU_OK with field NULL.
7. Integration test: `record_outbound_with_p_seth_persists_column` — drive
   a fake outbound through the daemon path, assert `p_seth_at_send` in the
   row is in [0, 1] (NOT -1.0).
8. Full suite passes.

**Files:**
- `include/human/agent.h` — add field to struct
- `src/agent.c` — init + deinit hooks
- `src/daemon.c` — replace -1.0 with scoring call
- `tests/test_agent.c` — 2 unit tests
- `tests/test_dpo.c` or new file — integration test

**Verifier evidence:**
```
./build/human_tests --filter=persona_eval 2>&1 | grep -cE "PASS"  # expect ≥10 (8 from R5-P5b + 2 new)
./build/human_tests --filter=record_outbound_with_p_seth 2>&1 | grep -E "PASS|FAIL"
./build/human_tests 2>&1 | grep "Results:"
# Production smoke (manual):
# 1. restart gateway
# 2. send one request
# 3. sqlite3 ~/.human/memory.db "SELECT p_seth_at_send FROM production_outcomes ORDER BY id DESC LIMIT 1;"
# Expect: a non-NULL value in [0, 1], NOT -1.0
```

---

## Sprint definition of done

- [ ] All 3 stories' acceptance criteria met
- [ ] All new unit tests pass
- [ ] Full suite passes (≥11582 + ~6 new tests)
- [ ] Verifier agent confirms each story's behavior empirically
- [ ] Critic agent reviews each story; no HIGH/CRITICAL findings
- [ ] Sprint auditor runs against this stories.md; result PASS or PASS_WITH_NOTES
- [ ] Closing commit tagged `v-sprint-46-close`

## Risks

| Risk | Mitigation |
|---|---|
| inbound iMessage handler is in a hot path; latency lookup may add SQL on every msg | Use the existing message_ref index (idx_po_msg_ref); single SELECT |
| Agent init failure due to missing model file would block all agent creation | Failure path returns HU_OK with field NULL; downstream gracefully degrades to 0.5 |
| Loading model on every agent creation if multiple agents are spawned | Load lazily on first score call; cache in agent struct |
| Existing record_outbound caller in daemon.c may need signature update across all build variants | Verify minimal/no-sqlite/cross-arm64 still compile |

## Out of scope (deferred to Sprint 47)

- R6.1, R6.2, R6.3 (uncertainty routing) — depends on R5.3 landing first
- Wiring best-of-N in production to actually populate `alternatives`
- L5 production shadow-mode (the original P5 from Round 3)
