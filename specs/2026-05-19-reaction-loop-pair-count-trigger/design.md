# Reaction Loop — DPO Pair-Count Auto-Training Trigger — Design

## Components

- **Pair-count gate** — new daemon tick check function `hu_daemon_tick_dpo_pair_count_trigger()` that reads the configured threshold, queries `dpo_pairs` count, and enqueues training when threshold is crossed. Lives in: `src/daemon.c` near the existing learner-pending gate at lines 3506-3511.
- **Shared training-runner entry** — small refactor of the existing learner-pending path so both triggers route through one function `hu_training_runner_enqueue_lora_persona()`. Lives in: `src/agent/lora_training_runner.c` (or wherever the existing learner-pending enqueue happens).
- **Coalescing guard** — daemon-resident "in-flight training" flag set by the runner on enqueue and cleared on completion; both triggers consult before enqueueing. Lives in: `src/agent/lora_training_runner.c`.
- **Silent-config-gated log emit** — `hu_log_info_once()` invocation in the new tick function on both disabled and enabled first-pass states, per `~/.claude/rules/silent-config-gated-subsystems.md`.
- **Config plumbing** — new key `learning.dpo_pair_training_threshold` (integer, default 100) added to the existing config parser. Lives in: `src/config_parse.c` + `include/human/config.h`.
- **E2E pair-count test** — extension of `test_e2e_rl_loop.c` (or new `test_e2e_rl_loop_pair_count.c`) that drives the loop via the new trigger.

## Data flow

```
[reaction arrives via existing reaction_handler]
   │
   ▼
[hu_dpo_record_pair → dpo_pairs table]    ─────  (no change in this spec)
   │
   ▼
[daemon tick fires]
   │
   ├──▶ [existing learner-pending check: ≥10 signals?]
   │         │
   │         └─▶ [if yes AND no in-flight training:
   │              hu_training_runner_enqueue_lora_persona(...)]
   │
   └──▶ [NEW: pair-count check: count(uncommitted dpo_pairs) ≥ threshold?]
             │
             └─▶ [if yes AND no in-flight training:
                  hu_training_runner_enqueue_lora_persona(...)]

[Either trigger → same enqueue function → existing trainer plumbing → existing hot-load]
```

## Decisions

- **D-RL-1 (AC-RL-1): Threshold defaults to 100, configurable.** Chose 100 over alternatives like 50 (too eager on small days), 25 (would trigger spuriously in tests), 500 (slow feedback loop). 100 reflects a "non-trivial signal mass": ~a week's worth of reactions for an active conversational user, ~a day for a heavy user. Configurable so operators on slower hardware can raise the threshold to amortize training cost.
- **D-RL-2 (AC-RL-2): Shared entry function, no parallel code path.** The existing learner-pending enqueue path is refactored into `hu_training_runner_enqueue_lora_persona(trigger_reason, ...)` and the pair-count check calls it with `trigger_reason="pair_count_threshold"`. Chose refactor over duplicate because two parallel paths drift — and the W4/W13 learner pipeline already has rich invariants we don't want to re-state.
- **D-RL-3 (AC-RL-3): Post-training hot-load is unchanged.** The existing `hu_lora_training_runner()` post-completion hot-load (`src/agent/lora_training_runner.c:386-417`) runs identically; pair-count trigger is purely a new ENTRY to the existing pipeline, not a new pipeline.
- **D-RL-4 (AC-RL-4): Silent-config-gated compliance via existing helper.** `hu_log_info_once()` is the project-standard mechanism; both the disabled-by-config case (`threshold == 0`) and the enabled-first-tick case emit one line each. The disabled line names the config key per rule (`learning.dpo_pair_training_threshold`).
- **D-RL-5 (AC-RL-6): Coalescing matches existing scheduler policy (no coalescing in this spec).** Recon Q-M3-B revealed the existing `hu_w14_scheduler_enqueue_lora` path does NOT coalesce or de-dup. Both learner-pending and pair-count triggers enqueue independently; the scheduler processes sequentially. **This contradicts AC-RL-6 as originally written** ("exactly one enqueue when both fire") — see scope tension flagged in the spec presentation. Two paths forward, requires user input:
  - **Option A (match existing):** Drop AC-RL-6's "exactly one" requirement; both enqueues are valid; scheduler runs both back-to-back. May produce one wasteful training cycle but matches existing behavior consistently.
  - **Option B (new coalescing):** Add a new in-flight flag mechanism (one bool + atomic) at the runner level. Both triggers consult before enqueueing; second-on-same-tick skips with a log line. This is a deliberate departure from existing learner-pending semantics — but applies the same coalescing to BOTH triggers (back-fix learner-pending too).
  - **Recommendation:** Option A for this spec — matches existing semantics, simpler implementation, accepts the rare duplicate-training cost. Option B becomes a separate spec if duplicate trainings prove costly in practice.
- **D-RL-6 (AC-RL-5): E2E test mirrors existing `test_e2e_rl_loop.c` shape.** Same fake-provider policy-signature scheme; only the trigger path differs. Chose extension over net-new file because the existing file already has the harness scaffolding; copy-pasting would diverge.
- **D-RL-7 (Constraint: HU_ENABLE_LEARNING gate).** When the feature flag is off, the new tick function is a stub returning `HU_OK` per `~/.claude/rules/test-source-gate-symmetry.md`. Tests for the trigger are gated symmetrically.

## Risks

- **Risk-RL-1 (D-RL-1): Default threshold 100 may incidentally trigger in some `human_tests` fixtures.** **Mitigation:** review test fixtures during implementation; if any test currently writes >100 dpo_pairs, either lower its fixture or override `learning.dpo_pair_training_threshold` for that test (the existing test harness has config-override scaffolding).
- **Risk-RL-2 (D-RL-2): Refactoring the existing learner-pending enqueue may regress its callers.** **Mitigation:** the refactor is a rename-and-add-parameter; all existing callers add the constant `trigger_reason="learner_pending"`. The compiler enforces caller updates.
- **Risk-RL-3 (D-RL-5): "Skip if in-flight" can mask a genuinely stuck runner.** If training hangs and the in-flight flag stays set, no new training runs ever fire. **Mitigation:** the existing runner already has a watchdog (the W13 trainer has a max-duration constraint); if missing, this spec adds a runner-side "training_in_flight_since" timestamp and the daemon force-clears the flag after `learning.dpo_training_max_duration_sec` (default 1800).
- **Risk-RL-4: The `dpo_pairs.count` query runs on every daemon tick.** Cheap (single SQLite `COUNT(*)` on an indexed table) but worth measuring. **Mitigation:** if profiling shows the count query is hot, cache the count and invalidate on insert. Defer until/unless profiling indicates a problem.

## Open design questions

- Q-RL-A: Is the "uncommitted" pair-count computed as `total - exported` (weekly export marker) or `total - last_training_consumed`? The latter is more accurate for triggering, but requires a new "consumed" marker. The former is simpler. Decide during implementation based on whether the trainer marks consumed pairs.
- Q-RL-B: Does the threshold check use global pair count, per-contact, or both? The learner-pending check is global per the recon — match its scope for consistency. If per-contact is desired later, that's a follow-up spec.
