# Reaction Loop — DPO Pair-Count Auto-Training Trigger — Requirements

## Goal

Wire `dpo_pairs.count` as a first-class auto-training trigger alongside the existing learner-pending-count trigger, so that reaction-derived feedback closes the personalization loop on the cadence of the user's reactions, not on the cadence of the orthogonal RL-signal collector.

Today, the reaction-collection → DPO-pair → training pipeline is fully operational, but training only fires when the *learner-pending* count crosses 10. The DPO-pair count is checked only by the weekly export job (Sunday 02:00 UTC), not by the training trigger. This means users can accumulate hundreds of reaction-derived DPO pairs without any adapter ever being retrained — the loop is closed in plumbing but not in cadence.

This spec adds a single new trigger path and proves it E2E.

## User stories

- As a user, I want my reactions to drive personalization on the cadence I'm actually using h-uman, so that a busy day of feedback closes the loop within hours, not weeks.
- As an operator, I want a configurable threshold on `dpo_pairs.count` so that I can tune the trade-off between training cost (which is non-trivial on Apple Silicon) and personalization freshness.
- As a developer, I want the pair-count trigger to share the existing training entry function with the learner-pending trigger so that we don't have two diverging code paths to maintain.

## Acceptance criteria

- [ ] **AC-RL-1: Threshold-gated trigger.** A new config key `learning.dpo_pair_training_threshold` (integer, default `100`, settable via `config.json`) controls a daemon-tick check: when uncommitted `dpo_pairs.count` ≥ threshold, the daemon enqueues a training run. Pinned by a unit test that exercises threshold-crossing in both directions (89 → 100 fires; 99 → 99 does not).
- [ ] **AC-RL-2: Shared entry function.** Both the existing learner-pending trigger and the new pair-count trigger call the same internal function `hu_training_runner_enqueue_lora_persona(...)` (rename of an existing function is acceptable). No new training-pipeline code path is introduced; only a new entry to the existing pipeline. Pinned by a test that asserts both triggers cause structurally identical task records in the runner queue.
- [ ] **AC-RL-3: Existing post-completion hot-load unchanged.** The existing `hu_lora_training_runner()` adapter hot-load at `src/agent/lora_training_runner.c:386–417` runs unchanged for pair-count-triggered training. Existing E2E test (`test_e2e_rl_loop.c`) continues to pass as-is; no regression.
- [ ] **AC-RL-4: Silent-config-gated compliance.** When `learning.dpo_pair_training_threshold` is set to `0` (operator explicitly disables), the daemon emits `hu_log_info_once(...)` on first tick naming the config key, per `~/.claude/rules/silent-config-gated-subsystems.md`. When enabled, an analogous one-shot info-level activation line fires on first successful threshold check (even if threshold not yet crossed).
- [ ] **AC-RL-5: E2E test via pair-count path.** A new test (`tests/test_e2e_rl_loop_pair_count.c` or extension of the existing E2E file) drives the loop via pair-count trigger specifically: injects N reactions, asserts pair count reaches threshold, asserts training enqueued, completes the existing fake-provider policy-signature-delta proof (`before_response != after_response`). Must NOT pass if the pair-count trigger silently no-ops.
- [ ] **AC-RL-6: No double-firing.** When BOTH triggers (learner-pending AND pair-count) would fire on the same daemon tick, only ONE training run is enqueued (the first to fire wins; the second observes the in-flight run and skips). Pinned by a test that arms both triggers simultaneously and asserts exactly one enqueue.

## Non-goals

- Replacing the learner-pending trigger. Both coexist; pair-count is additive.
- Cross-contact training batching. The pair-count check operates on the existing per-contact aggregation, or globally if the existing learner trigger does — match the existing semantic, don't redesign.
- Online learning during conversation (mid-turn adapter updates).
- Changing the DPO pair construction logic (`hu_reaction_handler_handle_event()`) — that pipeline is verified operational; this spec only adds a new training trigger downstream.
- Cross-provider training coordination (Gemini, Anthropic). This spec stays inside the existing local training path.
- UI / dashboard exposure of the trigger state. CLI / log inspection is sufficient.

## Constraints

- Reuses existing `hu_lora_training_runner()` and the W13/W14 trainer plumbing. No new training code path.
- Threshold default chosen so existing tests (`human_tests` suite, 11,359 tests) do NOT regress — i.e., test corpora should not cross the default 100-pair threshold incidentally.
- All ACs pinned by automated tests in `tests/`. No manual verification.
- Tests deterministic; existing reaction-handler test infrastructure reused (`test_reaction_handler_e2e.c` pattern).
- C11 `-Wall -Wextra -Wpedantic -Werror`, ASan-clean.
- The new trigger must respect the existing learning-disabled feature flag (`HU_ENABLE_LEARNING`); if the flag is off, the trigger is a stub that returns `HU_OK` (per `~/.claude/rules/test-source-gate-symmetry.md`).
