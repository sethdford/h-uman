# Reaction Loop — DPO Pair-Count Auto-Training Trigger — Tasks

| # | Task | ACs | Owner | Status |
|---|---|---|---|---|
| 1 | Add config key `learning.dpo_pair_training_threshold` (integer, default 100) to the config parser at `src/config_parse.c`. Update `include/human/config.h` struct. Add round-trip test (parse → struct → re-emit) and a test for "unknown key" warning regression. | AC-RL-1 | TBD | pending |
| 2 | Refactor existing learner-pending enqueue into a shared entry: rename or wrap the existing function in `src/agent/lora_training_runner.c` (or wherever the existing `hu_w14_scheduler_enqueue_lora` boundary is) into `hu_training_runner_enqueue_lora_persona(trigger_reason, ...)`. Update existing callers to pass `trigger_reason="learner_pending"`. | AC-RL-2 | TBD | pending |
| 3 | Implement `hu_daemon_tick_dpo_pair_count_trigger()` in `src/daemon.c`: queries uncommitted dpo_pairs count, compares to threshold, enqueues training via the shared entry from Task 2 when threshold is crossed. Trigger reason: `"pair_count_threshold"`. | AC-RL-1, AC-RL-2 | TBD | pending |
| 4 | Add `hu_log_info_once()` calls for: (a) `threshold == 0` (disabled by operator) naming the config key, (b) first successful threshold check post-startup (activation confirmation). Both per `~/.claude/rules/silent-config-gated-subsystems.md`. | AC-RL-4 | TBD | pending |
| 5 | Unit test threshold-crossing behavior: `tests/test_dpo_pair_count_trigger.c`. Cases: 89→100 fires; 99→99 does not; 0 (disabled) emits the info-once line; the trigger is a no-op under `HU_ENABLE_LEARNING=OFF`. | AC-RL-1, AC-RL-4 | TBD | pending |
| 6 | Shared-entry equivalence test: assert both triggers (learner-pending + pair-count) produce structurally identical runner queue records (same `target_model`, same training params, different `trigger_reason`). `tests/test_training_runner_shared_entry.c`. | AC-RL-2 | TBD | pending |
| 7 | E2E test via pair-count path: extend `tests/test_e2e_rl_loop.c` OR new `tests/test_e2e_rl_loop_pair_count.c`. Drives the loop specifically via pair-count trigger (NOT learner-pending). Asserts before-vs-after policy signature differs. | AC-RL-5 | TBD | pending |
| 8 | Regression check: existing `test_e2e_rl_loop.c` (learner-pending path) continues to pass unchanged. Existing `lora_training_runner` post-completion hot-load runs identically. No code changes required; this is a CI verification task. | AC-RL-3 | TBD | pending |
| 9 | **DEFERRED**: AC-RL-6 (coalescing of double-fires) is dropped from this spec. Existing scheduler has no coalescing (recon Q-M3-B confirmed); satisfying AC-RL-6 requires NEW behavior affecting both triggers. File a follow-up spec `specs/<date>-training-coalescing/` rather than entangling it here. | (none — deferred) | TBD | deferred |

## Dependencies

- Task 3 depends on Task 2 (shared entry must exist before new trigger calls it).
- Task 5 depends on Tasks 1, 3, 4 (exercises all of them).
- Task 6 depends on Task 2 (shared entry).
- Task 7 depends on Tasks 1, 2, 3 (full loop must exist before E2E).

## Sequencing recommendation

**Single phase, sequential:** 1 → 2 → 3 → 4 → (5, 6 in parallel) → 7 → 8.

No phase gating; all tasks are small. Total estimate: ~50-80 LOC change, ~150 LOC test.

## Cross-spec dependencies

- **Spec 1 AC-M3-7** (daemon auto-invocation of frontier MLX training) depends on this spec's AC-RL-1 landing. The pair-count trigger lights up the data path; Spec 1 routes the trigger to the frontier-model training target. Land Spec 2 before Spec 1's Phase D.
- No other cross-spec dependencies.

## Verification

After all tasks complete, spawn `spec-verifier`:
```
Agent({
  description: "Verify reaction-loop pair-count spec satisfaction",
  subagent_type: "spec-verifier",
  prompt: "Spec at specs/2026-05-19-reaction-loop-pair-count-trigger/. Verify AC-RL-1 through AC-RL-5 (AC-RL-6 is deferred). Pay special attention to the E2E test in Task 7 — it must drive via pair-count trigger, not via learner-pending. Output RESULT_spec-verifier=PASS|FAIL with per-AC evidence."
})
```

## Scope note (read before implementation)

AC-RL-6 from requirements.md is **deferred** per Task 9. Implementer should NOT add coalescing logic in this spec's scope. Two triggers firing on the same tick produce two enqueues; the existing scheduler runs them sequentially; one of the two training runs may be wasteful (same data, twice) but is otherwise harmless. Coalescing belongs in a separate spec that addresses both triggers symmetrically.
