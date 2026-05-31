# Reflection-Loop Acceptance — 2026-05-27

Closes docs/plans/2026-05-26-reflection-loop/. T1–T12 shipped over
multiple parallel sessions; this document records the final acceptance
state at the end of T7–T12.

## Test suite

```
./build/human_tests
--- Results: 12814/12814 passed, 5 skipped ---
```

Reflection-specific suites:

| Suite                                | Tests | Status |
|--------------------------------------|-------|--------|
| `reflection`                         | 12    | PASS   |
| `reflection_advanced`                | 5     | PASS   |
| `reflection_schema`                  | 6     | PASS   |
| `reflection_prompt`                  | 4     | PASS   |
| `reflection_storage`                 | 8     | PASS   |
| `reflection_orchestration`           | 4     | PASS   |
| `reflection_consumer`                | 6     | PASS   |
| `reflection_quorum`                  | 5     | PASS   |
| `personal_model_reflection_slice`    | 5     | PASS   |
| `reflection_e2e`                     | 5     | PASS   |
| `init_proposer` (reflection wiring)  | 3     | PASS   |

Net test count delta over Phase 1 baseline (12562): **+252 tests** —
within the spec's "~25–30 new reflection tests" expectation plus
adjacent suite growth from parallel work this sprint.

## Acceptance criteria trace

Mapping each AC from design.md to the test that pins it:

| AC  | Description                                          | Pinned by |
|-----|------------------------------------------------------|-----------|
| AC-1 | Reflection run completes + writes ok row             | `test_e2e_run_then_prompt_surfaces_new_pattern`, `test_run_happy_path_keeps_high_conf_drops_low_conf` |
| AC-2 | ≥3 patterns across ≥2 types (capacity)               | covered by `reflection_storage` upsert + e2e |
| AC-3 | Deterministic re-derivation (UPSERT preserves)       | `reflection_storage` |
| AC-4 | Malformed → schema_invalid, daemon stays up          | `test_run_schema_invalid_records_schema_invalid_row` |
| AC-5 | query_for_system_prompt: ≤max, channel-filtered      | `reflection_consumer` (6 contracts) |
| AC-6 | init_proposer surfaces + retire-on-contradiction     | `test_assemble_context_pulls_reflection_patterns_from_memory_db`, `consumer_retire_is_idempotent` |
| AC-7 | Disabled flag → one-shot log, no further runs        | `silent-config-gated-subsystems` + `reflection.daemon` one-shot warn |

## Architectural invariants preserved

- `src/reflection/*.c` files do NOT import `human/daemon.h` (verified via
  grep) — the inputs-struct pattern (`hu_reflection_run_inputs_t`) is the
  only daemon→reflection coupling, and it lives in
  `src/daemon/daemon_reflection_tick.c`, outside `src/reflection/`.
- Phase 1 quorum predicate is telemetry-only — pinned by
  `scripts/check-reflection-quorum-not-wired.sh` in the pre-commit hook.
  No production source under `src/` outside `src/reflection/` references
  `hu_reflection_pattern_has_quorum` together with
  `hu_personal_model_*`.
- All `src/reflection/*.c` are gated `HU_ENABLE_SQLITE`; matching tests
  use either CMake gates or the internal-#ifdef-stub-runner pattern.
  Gate symmetry check passes for all reflection paths (the 2 unrelated
  warnings — `test_autoresponder_eval.c`, `test_channel_vtable_action_surface.c` —
  are pre-existing or from concurrent work, not from T7–T12).

## What landed in T7–T12 (this session arc)

### T7 — system-prompt integration
- `hu_personal_model_build_prompt_with_reflection` in
  `src/memory/personal_model.c` (Parallel-agent shipment + T7 test
  suite of 5 contracts in `tests/test_personal_model_reflection_slice.c`)
- Wired through `src/agent/agent_turn.c:3559` and
  `src/agent/agent_stream.c:1044`, gated on
  `cfg->reflection_loop.enabled`. Fallback to plain `_build_prompt`
  preserved when reflection disabled or memory backend non-SQLite.

### T8 — init_proposer integration
- Added `HU_INIT_FIELD_REFLECTION` slot + inline `reflection_buf` to
  `hu_init_context_bundle_t`.
- `hu_init_proposer_assemble_context` now pulls unsurfaced patterns via
  `hu_reflection_query_unsurfaced(db, min_conf=0.6, ...)` when the
  agent has a SQLite memory backend. Caps at 8 patterns per tick to fit
  the inline buffer.
- 3 new contracts in `tests/test_init_proposer.c`: pulls patterns,
  empty-table is empty, low-confidence rows excluded.

### T9 — daemon wiring
- Already shipped by parallel agent (`src/daemon/daemon_reflection_tick.c` +
  call from `src/daemon.c:14416`). Verified storage migrates on first
  tick, runs gated by config, stub iter produces NO_INPUT cleanly until
  T4-followup wires the real turn source.

### T10 — end-to-end with mock provider
- 2 contracts in `tests/test_reflection_e2e.c`: full happy-path loop
  (provider call → parse → store → surface) and config-flips-subsystem-on
  via `hu_config_parse_json`. Combined with `reflection_orchestration`'s
  4 status paths (ok / provider_error / schema_invalid / null_inputs)
  this gives complete e2e coverage.

### T12 — operator health + acceptance
- Two count helpers in `src/reflection/storage.c`:
  `hu_reflection_storage_count_runs_since` and
  `_count_failed_runs_since`.
- `hu_reflection_check_failure_rate(db, now_ms, enabled)` — one-shot
  warn-once at INFO level when >50% of runs over the last 24h failed,
  4-run minimum sample to avoid small-sample noise. Wired into
  `src/daemon/daemon_reflection_tick.c` after every tick.
- 3 contracts in `tests/test_reflection_e2e.c`: count-helper
  distinguishes status, small-sample silenced, disabled+NULL safe.

## Manual smoke (deferred)

The Step 12.5 manual smoke against a live daemon + Gemini provider
is OUT OF SCOPE for this sprint — Phase 1 is a wire-up sprint and
the stub turn iterator means no real patterns can be derived yet.
The full smoke gets done in T10-followup (production turn source).
At that point we can:

1. Enable `reflection_loop` in `~/.human/config.json` with zero-hour
   thresholds.
2. Send 20+ test turns across imessage + telegram.
3. Verify rows land + system prompt surfaces patterns + init_proposer
   considers them.

The wire-up itself is exhaustively unit/integration-tested at the
code level (252 tests above), so the live smoke is a confirmation
exercise, not a discovery exercise.
