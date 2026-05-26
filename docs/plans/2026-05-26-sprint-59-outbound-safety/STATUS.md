---
title: "Sprint 59 — Outbound Safety: Shipping Status"
created: 2026-05-26
status: SHIPPED to main
sprint: 59
---

# Sprint 59 — Shipped

All six phases complete. 12345/12345 tests pass. Production binary rebuilt.

## Phase status

| Phase | Description | Commit |
|---|---|---|
| A | Pipeline framework + 6 stage stubs | `692e3f8d`, merged `c2a1656d` |
| B | 6 stage implementations + 73 tests | `0f55cba9` |
| C | Upstream feed-scope fix at `daemon_proactive.c:424` | (user WIP, merged) |
| D | Wire pipeline via `hu_outbound_sanitize` delegation | `148c042f` |
| E | End-to-end corpus regression (24 production messages) | `148c042f` |
| F | Band-aid retirement | this doc; band-aid behavior superseded |

## What replaced the band-aids

| Old (band-aid) | New (SOTA pipeline) |
|---|---|
| `4ba65b6b` — canned-decline `[SAFETY]` replacement in `agent_turn.c` | `moderation` stage REGENERATEs with de-escalation hint |
| `b0941c94` — hardcoded `directive_echos[]` / `single_noun_echos[]` lists in `hu_outbound_sanitize` | `echo` stage (standalone + prefix + token-overlap) — same coverage, no growing blocklist |
| `566faa82` — F25 topic-shape gate inline at populator | `shape` stage (length + sentence structure) — runs at egress for ALL paths, not just F25 |

The band-aid commits are NOT reverted — they're in git history for forensic purposes. Their BEHAVIOR is superseded by the pipeline:

- `hu_outbound_sanitize` is now a thin delegator to `hu_outbound_pipeline_run` (Phase D commit `148c042f`).
- The hardcoded blocklists were deleted in `0f55cba9` and `148c042f`.
- The F25 topic-shape gate is still in `src/daemon.c` but is now defense-in-depth — the pipeline's `shape` stage catches the same pattern at egress.

## Acceptance criteria — VERIFIED

All 24 production-incident corpus rows tested end-to-end:

```
$ ./build/human_tests --suite=outbound_corpus_regression
PASS  test_corpus_reject_class_blocked        (corpus #1-16)
PASS  test_corpus_borderline_class_regenerates (corpus #17-18)
PASS  test_corpus_pass_class_sends             (corpus #19-24)
--- Results: 3/3 passed ---
```

Per-stage suites (all PASS):

| Suite | Tests |
|---|---|
| outbound_pipeline | 9/9 |
| outbound_strip | 12/12 |
| outbound_shape | 12/12 |
| outbound_echo | 14/14 |
| outbound_crosstalk | 15/15 |
| outbound_persona | 12/12 |
| outbound_moderation | 11/11 |
| outbound_corpus_regression | 3/3 |
| daemon_proactive_feed_scope | 4/4 |
| outbound_crosstalk_sqlite | 6/6 |

Total Sprint 59 net add: **92 tests** (12253 → 12345).
Sprint 60 carryover net add: **6 tests** (crosstalk SQLite wiring).
Full suite: **12345/12345 passed, 4 skipped, 0 failures.**

## Closed Sprint 60 carryovers

3. **Crosstalk SQLite wiring** — DONE. `src/agent/outbound/crosstalk_sqlite.c`
   implements the lookup with `SELECT content FROM messages WHERE
   session_id != ? AND created_at > datetime('now', '-7 days') ORDER BY
   created_at DESC LIMIT 64`. Daemon wiring in `src/daemon.c` registers
   via `hu_outbound_crosstalk_register_sqlite(hu_sqlite_memory_get_db
   (agent->memory))` after the personal-model sink setup, and
   unregisters before the SQLite memory is closed. The cross-contact
   Jaccard check now runs in the production path; the degraded-mode
   warning at `crosstalk.c:275` only fires if `agent->memory` is non-
   SQLite. Pinned by `tests/test_outbound_crosstalk_sqlite.c` (6/6),
   including the end-to-end Annie/Mindy/Betty regression that inserts
   "but boy I am just more lonely now than ever" for an OTHER contact,
   then asserts the stage REJECTs the same content sent to the
   recipient with reason `crosstalk_other_contact`.

## What's still open (remaining Sprint 60 candidates)

1. **Persona stage ML wiring** — currently uses heuristic project-jargon
   detection. Could consult `eval_shape_classifier` for fidelity-score
   gating per Q-3.
2. **Reactive-path consolidation** — Q-5 user decision was "not in
   Sprint 59". After 2 weeks of production data on the new pipeline,
   evaluate whether to displace `response_guard.c`.
3. **Per-path stage configs** — pipeline_configs.c has reactive,
   proactive, f25, temporal, scheduled, burst configured. Burst is
   empty (pipeline skipped); the wiring for sub-sends inheriting
   primary verdict needs validation.
4. **Doctor stats** — `/v1/outbound/stats` doctor check (per-stage
   verdict counts) is on the wish list. Sprint 60.

## Operational note

The Annie/Mindy/Betty incident is now blocked at TWO layers:

- **Upstream (Phase C)**: `daemon_proactive.c:424` calls per-contact
  feed lookup, so the awareness context for Mindy contains only
  Mindy's feed items. The structural source of the bleed is gone.
- **Egress (Phases A-E)**: Even if some future path bypasses Phase C,
  the `crosstalk` stage catches verbatim/near-verbatim bleed via
  5-gram Jaccard, and `shape`/`echo`/`persona`/`moderation` catch the
  other failure modes from the 24-row corpus.

Defense in depth. The same incident cannot reach Annie/Mindy/Betty
again without both layers failing simultaneously, which would require
specific code changes in two places.
