# Sprint 38 — Global-state test hygiene + G8 biography + reject telemetry

**Branch:** `sprint-38-global-state-and-g8-bio`
**Sprint goals:**

1. **Global-state test hygiene** — harden tests that touch process-wide
   singletons so they don't depend on cumulative suite order.
2. **G8 biography** — extend persona identity echo to also scan
   `persona->biography` (longer, richer than `identity` / `core_anchor`).
3. **Reject telemetry** — lightweight counters for G5–G8 REJECTs so
   operators can measure hit rates before tuning thresholds.

## Stories

### S1 — Global-state hygiene

- `test_gateway_extended.c::test_rpc_models_decisions_returns_valid_json`:
  reset global route log before seeding (same pattern as Sprint 37 fix).
- Add `tests/test_global_state_hygiene.c` with:
  - `route_global_log_reset_yields_empty_count`
  - documents the contract: any test touching `hu_route_global_log()`
    must call `hu_route_log_init(log)` first.

### S2 — G8 biography

- `hu_guard_context_t` adds `persona_biography` + `persona_biography_len`.
- Refactor G8 sliding-window into `hu_guard_persona_text_echo(text, len, s, len)`.
- `hu_guard_has_persona_identity_echo` checks identity AND biography.
- Wire all 3 call sites: set biography from `agent->persona->biography`.

### S3 — Reject telemetry

- `hu_guard_reject_stats_t` + `hu_guard_reject_stats_snapshot` +
  `hu_guard_reject_stats_reset` in `response_guard.h`.
- Increment atomically in `hu_response_guard_check_ex` on REJECT per flag.
- Unit test: G8 reject increments `persona_identity_echo` counter.

## Definition of Done

- All stories shipped.
- 10334 + N tests pass, 0 ASan errors.
- Tagged `v-sprint-38-close`, cherry-picked to main.

## Out of scope

- Quality gate MARGINAL→REJECT (needs runtime measurement).
- Per-channel G5 thresholds.
- Widen G7 lookahead.
