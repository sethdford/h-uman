# Self-Model Scaffold — Tasks

| # | Task | ACs | Owner | Status |
|---|---|---|---|---|
| 1 | Define `HU_ENABLE_SELF_MODEL` CMake option (default OFF). Wire into existing build-variant pattern alongside `HU_ENABLE_ML`, `HU_ENABLE_LEARNING`. Update `.github/workflows/ci.yml` (or equivalent) to include a disabled-variant build. | AC-SM-6 | TBD | pending |
| 2 | Create header skeleton `include/human/agent/self_model.h` and source skeleton `src/agent/self_model.c`, both wrapped in `#ifdef HU_ENABLE_SELF_MODEL` per `~/.claude/rules/test-source-gate-symmetry.md`. Define `hu_agent_behavior_log_t`, `hu_agent_self_observation_t`, `hu_agent_self_concern_t` structs. | AC-SM-1 | TBD | pending |
| 3 | Implement `hu_agent_behavior_log_t` ring buffer: fixed-capacity (default 256, settable via `self_model.behavior_log_capacity`), zero-alloc per record, monotonic head index, atomic head increment if multi-threaded. | AC-SM-1 | TBD | pending |
| 4 | Implement `hu_agent_behavior_log_record(...)` invoked from inside `hu_agent_m3_on_provider_success()` (one call site inside that function — NOT scattered across the 11 sites that call on_provider_success). Reads `emotional_register` from the now-populated world model. | AC-SM-1, AC-SM-2 | TBD | pending |
| 5 | Build-blocking grep test: `tests/test_self_model_single_write_site.c` (or shell-based check in CI) that fails if `hu_agent_behavior_log_record(` appears more than once outside `tests/`. Allowlist file in tests/ for known exception. | AC-SM-2 | TBD | pending |
| 6 | Schema migration adding `agent_self_observations` and `agent_self_concerns` tables (see design for SQL). Add to the existing migration runner; bump schema version. | AC-SM-3, AC-SM-5 | TBD | pending |
| 7 | Implement `hu_daemon_tick_self_observation_aggregate()`: triggers every 100 turns OR 60 minutes (configurable), computes aggregates over the recent ring window, INSERT INTO agent_self_observations. | AC-SM-3 | TBD | pending |
| 8 | Compute drift signal per dimension: σ vs. calibrated baseline (existing `hu_calibration_*`). When |σ| ≥ `self_model.drift_threshold_sigma` (default 2.0) AND baseline N ≥ `self_model.drift_minimum_baseline_n` (default 50), INSERT INTO agent_self_concerns. | AC-SM-5 | TBD | pending |
| 9 | Extend `hu_self_model_t` (existing struct at `include/human/agent/world_model.h:196-209`) with `recent_self_observations[4]` fixed array. Backward-compatible: existing fields unchanged. | AC-SM-4 | TBD | pending |
| 10 | Implement `hu_world_model_merge_self_observations()` in `src/agent/world_model_bridge.c`. Reads top-4 most-recent rows from agent_self_observations, populates the new field. Called from the existing merge orchestrator alongside `merge_self_emotion`, `merge_self_recent_tools`. | AC-SM-4 | TBD | pending |
| 11 | Privacy-hygiene grep test: `tests/test_self_model_no_content_capture.c`. Fails build if `src/agent/self_model.c` or `include/human/agent/self_model.h` reference disallowed field names (`body`, `content`, `message`, `text`, `arg`, `prompt`, `response`) — with an allowlist for `response_length`, `response_latency_ms`. | AC-SM-7 | TBD | pending |
| 12 | Unit tests for: (a) ring buffer wrap-around correctness, (b) aggregation determinism over fixed window, (c) drift detection threshold semantics, (d) zero-cost when flag OFF (assembly inspection or stub-detection). | AC-SM-1, AC-SM-3, AC-SM-5, AC-SM-6 | TBD | pending |
| 13 | CI variant binary-size check: assert disabled-flag build's binary size grows by less than 1 KB versus pre-spec baseline. Lives in: existing benchmark workflow. | AC-SM-6 | TBD | pending |

## Dependencies

- Tasks 2-13 all depend on Task 1 (flag exists).
- Task 4 depends on Tasks 2, 3 (struct + buffer).
- Task 5 depends on Task 4 (something to grep for).
- Tasks 7, 8 depend on Tasks 3, 6 (buffer to read, table to write).
- Task 10 depends on Tasks 6, 9 (table to query, struct field to populate).
- Task 13 depends on all code tasks (measures the impact).

## Sequencing recommendation

**Phase A (infrastructure):** 1, 2, 3 — types and skeleton.
**Phase B (recording):** 4, 5, 11 — write path + invariants.
**Phase C (aggregation):** 6, 7, 8 — periodic compute + persistence.
**Phase D (integration):** 9, 10 — world-model surface.
**Phase E (verification):** 12, 13 — tests + binary-size check.

## Cross-spec dependencies

- **Originally noted dependency on Spec 1 AC-M3-2 is REMOVED.** Recon confirmed `hu_agent_m3_on_provider_success()` is already called from 11 sites covering both streaming and non-streaming paths. Spec 3 hook lives inside that function and is fully covered today.
- No active cross-spec dependencies. Spec 3 can implement in parallel with Spec 1, 2, 4.

## Verification

After all tasks complete:
```
Agent({
  description: "Verify self-model scaffold spec satisfaction",
  subagent_type: "spec-verifier",
  prompt: "Spec at specs/2026-05-19-self-model-scaffold/. Verify AC-SM-1 through AC-SM-7. AC-SM-6 requires the disabled-flag variant build to pass with binary-size delta < 1 KB. AC-SM-7 requires the privacy grep test to actually run in CI. Output RESULT_spec-verifier=PASS|FAIL."
})
```
