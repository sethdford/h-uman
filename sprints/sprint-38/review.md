# Sprint 38 — Review

**Branch:** `sprint-38-global-state-and-g8-bio`
**Stories:** S1 (global-state hygiene), S2 (G8 biography), S3 (reject telemetry).

## What shipped

### S1 — Global-state test hygiene

- `route_global_log_reset_yields_zero_count` — contract test: `hu_route_log_init`
  on the global log yields count 0, survives a route + reset cycle.
- `test_gateway_extended.c::test_rpc_models_decisions_returns_valid_json` —
  resets global log before seeding (same pattern as Sprint 37's
  `route_populates_global_log` fix).

### S2 — G8 biography

- `hu_guard_context_t` adds `persona_biography` + `persona_biography_len`.
- G8 refactored: shared `hu_guard_persona_text_echo(text, len, ...)` helper;
  `hu_guard_has_persona_identity_echo` checks identity AND biography.
- All 3 call sites populate biography from `agent->persona->biography`.
- Tests: biography-only reject, orthogonal biography vs identity.

### S3 — Reject telemetry

- `hu_guard_reject_stats_t` with counters for semantic / length / director /
  persona_pii / persona_identity (G8).
- `hu_guard_reject_stats_snapshot` + `hu_guard_reject_stats_reset`.
- Atomics in `response_guard.c`; incremented on Phase 3 + Phase 4 REJECT.
- Tests: G8 increment + reset clears.

## Test results

- Response Guard: 81/81 (was 77; +4).
- Model Router: 58/58 (was 57; +1).
- Full dev: **10339/10339** (was 10334; +5 = matches).
- 0 ASan errors. Clean `-Wall -Wextra -Wpedantic -Werror`.

## Demo

Biography-only G8 (no identity set):

```
PASS  guard_g8_rejects_biography_only_echo
```

Telemetry:

```
PASS  guard_reject_stats_g8_increments_on_identity_echo
PASS  guard_reject_stats_reset_clears_counters
```
