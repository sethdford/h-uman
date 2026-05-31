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

## Task 0.1 — `hu_service_run` internal phase map (the carve seams)

`hu_service_run` (L2496→EOF) is ONE function with a clean 3-part macro-structure.
The mass is concentrated in the per-channel processing loop.

| Phase | Lines | ~LOC | Contents | Carve target |
|---|---|---|---|---|
| **SETUP** | L2496–3337 | ~840 | config/binding init (W4/W7/W14 @L2754, LoRA auto-load @L3038, M3 Bridge @L3068), per-channel setup loop (L3204), thought-store + counter init (L3270–3283) | `daemon/service_setup.c` |
| **MAIN TICK LOOP** | L3337–14670 | **~11,300** | `while (!HU_STOP_FLAG)` @L3337 | (decompose, below) |
| ↳ idle ticks | L3338, L3371 | small | intrinsic-motivation tick, C-series prosocial tick (both default-OFF) | `tick_scheduler.c` |
| ↳ proactive checkins | L3463 | call | `hu_service_run_proactive_checkins(...)` (already a function) | already extractable |
| ↳ **per-channel processing** | **L5039–~14340** | **~9,300** | `for (i in channel_count)` — full inbound message processing for one channel. THE dominant mass. | `inbound_pump.c` (needs its OWN sub-inventory) |
| ↳ periodic subsystem ticks | L14348–14620 | ~270 | channel-health (L14348), observer registry (L14357), imessage Phase-3 (L14431), TOM-expectation GC (L14448), self-model aggregation (L14465), social tick (L14523) — each a cohesive block | `tick_scheduler.c` |
| **TEARDOWN** | L14675–14745 | ~70 | thought-store cleanup, detach personal-model sinks | stays in orchestrator |

**3 loops total** (per-channel setup @L3204, the main `while` @L3337, per-channel
processing `for` @L5039); **72 `HU_IS_TEST` blocks** inside the function.

### Recommended carve order (cohesive edges first, dominant mass last)
1. Extract the **periodic subsystem ticks** (L14348–14620) → `tick_scheduler.c` — 6 cohesive, near-independent blocks; lowest risk, immediate ~270 LOC out.
2. Extract **SETUP** (L2496–3337) → `service_setup.c` — runs once, clear boundary.
3. Extract the **idle ticks** → fold into `tick_scheduler.c`.
4. Extract the **per-channel processing body** (L5039 for-loop) → `inbound_pump.c`. **Do a dedicated sub-inventory of THIS block first** (it's 9,300 LOC — likely 5–8 further sub-functions: ingest → classify → route-to-agent → humanize → dispatch).
5. Residual `hu_service_run` = thin orchestrator: `setup()` → `while(!stop){ idle_ticks(); for(ch) inbound_pump(ch); periodic_ticks(); }` → `teardown()`. Target < 800 LOC.

**Risk note:** every extracted phase reads the loop's local state (the per-tick
context, channel array, agent, config). Thread these as an explicit
`hu_service_loop_ctx_t*` struct (heap-allocated per the
`asan-pthread-stack-aliasing-darwin` rule if any crosses a thread — though
`pthread_create=0` here, so likely safe stack-passing). Define the ctx struct in
`daemon/common.h` in the FIRST carve chip.

## Task 0.2 — per-channel body (`inbound_pump`) sub-map

The 9,300-LOC per-channel `for`-loop body (L5039–14340) is a **numbered
context-assembly pipeline → `hu_agent_turn` → post-process**. The inline comments
already number the stages — these are the sub-function seams:

| Stage | Line | Future sub-function |
|---|---|---|
| 1. Per-contact profile (persona) | L6309 | `inbound_build_contact_profile` |
| 2. Conversation history (channel vtable) | L6507 | `inbound_load_history` |
| Phase 6: prefix context | L6621 | `inbound_build_prefix_ctx` |
| 3. Awareness context (shared analyzer) | L7873 | `inbound_build_awareness` |
| 6. Attachment context | L8706 | `inbound_build_attachment_ctx` |
| 4. Response constraints (channel vtable) | L9187 | `inbound_build_constraints` |
| 5. Anti-repetition | L9248 | `inbound_anti_repetition` |
| 6. Relationship-tier calibration | L9275 | `inbound_calibrate_tier` |
| 7. Emotional topic map | L9404 | `inbound_build_emotion_map` |
| → agent turn | (5× `hu_agent_turn`) | the call into `src/agent/` |
| Phase 7: post-conversation episode | L11835 | `inbound_record_episode` |
| Phase 9: interaction quality | L11859 | `inbound_record_quality` |

So `inbound_pump.c` itself decomposes into ~10 single-purpose context-builders +
the agent call + 2 post-processors. Each builder is independently characterizable
(input: history/profile; output: a context struct field). **This is the E2
end-game** — but only after the cohesive edges (ticks, setup, teardown) are out
and the characterization harness is green.

---

**E2 Task 0 status: COMPLETE.** Function inventory ✓, static map ✓, threading
model ✓, `hu_service_run` phase map (0.1) ✓, `inbound_pump` sub-map (0.2) ✓.
**Next: E2 Task 1 (characterization harness) — requires the post-E1 `main` base +
a build, so it begins after PR #218 merges.**

See [phase-E2-daemon-service-lifecycle.md](phase-E2-daemon-service-lifecycle.md) for the full plan.
