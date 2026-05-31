---
title: E2 daemon.c Inventory (Task 0)
description: Fresh function/static inventory of src/daemon.c, the prerequisite for the E2 service_lifecycle carve. Replaces v1's stale line numbers.
status: draft
created: 2026-05-31
---

# E2 — `daemon.c` Inventory (Task 0 deliverable)

> **Measured 2026-05-31** against `src/daemon.c` = **14,745 LOC** (on `main`).
> This replaces every line number in the v1 Phase-2 plan, which cited
> `hu_service_run` at "L2495 / 3,759 LOC" — both wrong now. Per
> `~/.claude/rules/audit-verify-before-allege.md`, no E2 extraction chip runs on
> unverified offsets; this doc is the source of truth.

## Headline finding — the giant is bigger than v1 assumed

| Function | Start | Span (next-toplevel gap) | v1 claim |
|---|---|---|---|
| `hu_service_run` | **L2496** | **→ EOF, ~12,200 LOC** | "3,759 LOC" ❌ |
| `hu_service_run_proactive_checkins` | **L714** | **~1,497 LOC** | "1,496 LOC" ✓ |

`hu_service_run` is either a single ~12K-LOC function or contains static helpers
the column-0 grep missed. **E2 Task 0.1 (do FIRST): sub-inventory the internal
structure of `hu_service_run`** — map its `while`-loop phases (inbound intake,
tick scheduler, outbound flush, per-subsystem ticks) and the `HU_IS_TEST` blocks,
so the carve targets real seams, not arbitrary line cuts.

## Cohesive functions already extractable (smaller buckets)

| LOC | Start | Function |
|---|---|---|
| 153 | L229 | `daemon_reply_dedup_mark` (+ `_path` L208, `_ensure_loaded` L216) — reply-dedup bucket |
| 145 | L518 | `store_conversation_summary` |
| 102 | L2394 | `daemon_outbound_bus_cb` |
| 101 | L2293 | `daemon_stream_event_cb` |
| 62 | L422 | `record_topic_baselines_from_text` |
| 40 | L382 | `classify_comfort_response_type` |
| 34 | L484 | `score_comfort_engagement` |
| 34 | L2211 | `service_signal_handler` |
| 52 | L156 | `hu_style_clone_from_history` |

## File-scope statics (the carve's hard constraint — cross-unit state)

| Static | Line | Likely owner unit |
|---|---|---|
| `g_reply_dedup`, `g_reply_dedup_loaded` | 205-206 | reply-dedup (already partly in `src/daemon/reply_dedup.c`) |
| `g_autoresponder_cfg`, `g_autoresponder_loaded` | 254-255 | service loop |
| `k_daemon_configs[]` + `get_active_daemon_config()` | 293, 313 | shared accessor (export, don't duplicate) |
| `g_proactive_ctx` | 688 | proactive_checkins |
| `g_proactive_throttle` + `_initialized` + `daemon_throttle()` | 693-696 | peripheral_gov (accessor already exists) |
| `g_stop_flag` (`volatile sig_atomic_t`) | 2209 | service loop / signal handler |
| `g_empty_agent_response_streak` | 2494 | service loop |

Resolution pattern (from v1 Phase 2): single-bucket statics move with their unit;
cross-bucket statics become `daemon/common.h` externs or get an exported accessor
(never duplicated). `gov_budget` mutation crosses the peripheral_gov boundary →
add an explicit `hu_daemon_gov_*` mutator API (E2 Task 2) rather than direct writes.

## Threading model

`pthread_create` in `daemon.c` = **0**. The service loop does NOT spawn threads
itself (workers live in `src/agent/`). **E2 is function decomposition, not
concurrency surgery** — no data-race risk from the moves. Record this so reviewers
don't fear races.

## Test instrumentation

**88 `HU_IS_TEST` guards** in `daemon.c` — rich hooks for the characterization
harness (E2 Task 1). They must move **verbatim** with their functions.

## E2 execution order (revised from this inventory)

0. **Task 0.1 — sub-inventory `hu_service_run`'s internal phases** (NEW, blocking).
1. Characterization harness for one service-loop tick (uses the `HU_IS_TEST` single-tick hooks).
2. `hu_daemon_gov_*` mutator API (decouple gov_budget writes).
3. Extract `proactive_checkins` (~1,497 LOC, most self-contained) → `src/daemon/proactive_checkins.c`.
4. Extract the cohesive small buckets (dedup, summary, bus callbacks, signal handler).
5. Decompose `hu_service_run` along the phase seams found in Task 0.1 → `service_loop.c` (thin orchestrator) + `inbound_pump.c` / `outbound_pump.c` / `tick_scheduler.c`.
6. Verify `daemon.c` < 800 LOC; lower `MAX_BASELINE` (`check-file-size-ceiling.sh`).

See [phase-E2-daemon-service-lifecycle.md](phase-E2-daemon-service-lifecycle.md) for the full plan; this inventory fills its Task 0.
