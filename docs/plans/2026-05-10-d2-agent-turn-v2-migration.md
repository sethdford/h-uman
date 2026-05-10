---
date: 2026-05-10
status: deferred
risk: medium
scope: src/agent/agent_turn.c, src/agent/agent_stream.c, src/memory/, src/agent/world_model_bridge.c
---

# D2 — Migrate `agent_turn.c` memory recall to the v2 W7 facade

## Background

`hu_agent_turn` and `hu_agent_turn_stream_v2` currently use **two parallel
memory recall surfaces** when assembling the system prompt for a turn:

| Surface | Entrypoint | What it returns | When introduced |
|---------|-----------|-----------------|-----------------|
| **v1 contact recall** | `hu_memory_recall_for_contact()` (`src/memory/sqlite.c`) | Contact-scoped entries from the SQLite `entries` table, scored by recency + a small relevance heuristic | Pre-v2 baseline |
| **v2 world model** | `hu_w7_render_world_model()` (`src/agent/world_model_bridge.c`) | Per-contact snapshot of goals, negatives, theory-of-mind, decision style | W9 + bridge in FIX 12 |

These were initially expected to be a transitional state — the v2 facade
would subsume the v1 path and `hu_memory_recall_for_contact` would
disappear from `agent_turn.c`. After landing W7-W12 + the world-model
bridge, **the hybrid is now intentional**:

- The v1 path answers _"what did we say to this person before?"_ (verbatim
  prior conversation snippets)
- The v2 path answers _"what do we know about this person?"_ (structured
  beliefs, decisions, preferences)

They compose without collision and are merged into the prompt by the
section appenders in `agent_turn.c` and `agent_stream.c`. Removing
either today regresses the prompt quality.

## Why we are not migrating now

1. **Behavioral parity is unverified.** The W7 read path
   (`hu_memory_read` over `hu_memory_query_t`) has not been benchmarked
   for recall accuracy against the v1 entries table on a real 10K+ row
   corpus. A naive switch could regress contact-recall coverage.

2. **There are 14 other v1 callers.** A grep for
   `hu_memory_recall_for_contact` in `src/` shows callers in
   `agent_stream.c`, `dispatcher.c`, `proactive.c`, the channel
   handlers, and several scheduler/eval paths. A clean migration must
   handle all of them or accept divergence — which is worse than the
   current consistent hybrid.

3. **Type collision is contained.** The W7 type collision (legacy
   `hu_memory_t` vs W7 `hu_memory_t`) is currently dodged by the
   bridge pattern from FIX 12 (see
   `docs/plans/2026-05-10-w7-type-collision-cleanup.md`). Migrating
   `agent_turn.c` to call W7 directly would re-expose the collision in
   a high-traffic file and is sequenced after the rename.

4. **The hybrid is shipping.** As of 2026-05-10 the production daemon
   has been running on the hybrid path through the production fixes
   (FIX 10, 16, 17, 18, 19) without observed regressions. There is no
   open user complaint that the hybrid is incorrect.

## Migration trigger criteria

This migration is unblocked when ALL of:

- **W7 read benchmark**: `hu_memory_read` over a 10K-row corpus is
  measured at ≤1.5× v1 `hu_memory_recall_for_contact` latency
  (`scripts/benchmark-w7-read.sh` — TBW)
- **Type collision resolved**: One of the three rename options in
  `docs/plans/2026-05-10-w7-type-collision-cleanup.md` is chosen and
  applied
- **Eval suite stable**: A new eval suite scoring contact-recall
  accuracy against a labeled set lands and is green for both v1 and v2
  paths (so the migration can be A/B'd)
- **All v1 callers inventoried**: a `grep -rn hu_memory_recall_for_contact src/`
  audit produces a list with each caller's intended migration plan

When any of those are met independently they are useful by themselves;
all four together unblock the actual code change.

## Migration plan (when triggered)

Phase 1 (in-place):
1. Add `agent->w7_facade && agent->use_w7_recall` guard around the v1
   call in `agent_turn.c` and `agent_stream.c`.
2. Behind the guard, call `hu_memory_read` with a `hu_memory_query_t`
   shaped from the contact_id + msg + top_k = 5.
3. Convert returned `hu_memory_record_t[]` into the same prompt-section
   format the v1 path produces.
4. A/B in production for one week with `use_w7_recall=true` for 10% of
   contacts; compare prompt-quality metrics + user feedback.

Phase 2 (removal):
1. Flip the guard default to true.
2. After two weeks of green telemetry, delete the v1 branch and
   `hu_memory_recall_for_contact` itself.

## Decision

**Defer** until the trigger criteria are met. The current hybrid is
documented in `agent_turn.c` (search for "Memory recall: hybrid v1 + v2
by design") so future readers do not mistake it for a half-finished
migration.

## Tracking

| Trigger | Owner | Status |
|---------|-------|--------|
| W7 read benchmark | M2 workstream | Pending |
| Type collision resolved | M5 (HuLa) infrastructure | Pending |
| Eval suite stable | W16 evaluation backends | Pending |
| Caller inventory | This document | Pending |
