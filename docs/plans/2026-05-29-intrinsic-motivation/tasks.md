# Intrinsic Motivation (A3) — Tasks

> Follows `requirements.md` + `design.md`. ⚠⚠ T0 (threat model) HARD-gates all
> code. Highest-risk epic — every task subordinate to bounded/preemptible/
> auditable/cannot-harm-user.

## T0 — Threat-model review gate (BLOCKS all code) → AC-8, constraints
- [ ] Threat model covering: resource exhaustion, unwanted proactivity, acting
      during a user turn, goal drift toward self-interest. Read
      `docs/standards/security/`, `docs/standards/ai/ai-safety`.
- [ ] Prove no intrinsic goal can act against the user (internal+propose only).
- [ ] Append to `design.md`. Sign-off required before T1+.

## T1 — Drive dynamics (pure) ⓣ → AC-1
- [ ] `intrinsic_drive.h` + `hu_intrinsic_drive_tick`. Curiosity/boredom rise on
      inactivity/repetition, decay on satisfaction. NOT user-task derived.
- [ ] Tests pin rise + decay; deterministic (now passed in, no wall-clock calls).

## T2 — Start predicate (pure) ⓣ → AC-6, AC-4
- [ ] `hu_intrinsic_should_start`. Truth table incl. the load-bearing
      `user_active ⇒ false` preemption row + budget-exhausted + rate-limit rows.

## T3 — Self-originated goal + isolation-from-autonomy ⓣ → AC-2
- [ ] `hu_intrinsic_make_goal` tagged `origin=intrinsic_curiosity`.
- [ ] Test: distinct from every `hu_autonomy_generate_intrinsic_goal` branch
      (which stays user-reactive).

## T4 — Bounded runner ✅ DONE (2026-05-29) → AC-3, AC-4, AC-7
- [x] `hu_intrinsic_run_tick` (config-gated; per-tick budget cap; calls
      start-predicate; fills audit string + emits audit log; advances
      last_intrinsic_ts). `src/agent/intrinsic_drive.c`.
- [x] Tests: `runner_disabled_is_noop`, `runner_enabled_ripe_starts_and_audits`
      (audit contains origin+outcome), `runner_user_active_skips` (preemption),
      `runner_below_tick_budget_skips` (budget cap). All PASS.
- [x] DONE (2026-05-29, daemon wire): `hu_intrinsic_drive_t intrinsic_drive`
      added to the agent struct; `daemon.c` marks user activity
      (`hu_intrinsic_drive_tick(.., true, ..)`) at the reactive send site and,
      on a 5-min idle cadence beside the personal-model idle-decay block, ages
      the drive (`.., false, ..`) + calls `hu_intrinsic_run_tick` with
      config-sourced `enabled`/`per_tick_token_budget`, `user_active=false`,
      and a granted per-tick budget. Default-OFF (nothing runs until
      `intrinsic.enabled=true`). Build + full suite 13154/13154 + 0 ASan.
      Live daemon-turn FIRING still unexercised by tests (heavy loop path) —
      pairs with the live threat-model re-review before enabling in prod.

## T5 — Share only via init_proposer ⓣ → AC-5
- [ ] Shareable exploration output routed through existing `hu_init_proposer`
      (≥0.85). Test: intrinsic share cannot bypass the proposer gate (no
      alternate egress path exists).

## T6 — Config gate + one-shot log ✅ DONE (2026-05-29) → AC-8
- [x] `hu_intrinsic_config_t {enabled (default false), per_tick_token_budget}`
      in `config_types.h` + field in `config.h` + `parse_intrinsic` in
      `config_parse.c` (wired beside `parse_learning`). Config suite 502/502.
- [x] One-shot disabled log in `hu_intrinsic_run_tick` naming
      `cfg.intrinsic.enabled` (`silent-config-gated-subsystems.md`).

## T7 — self_direction eval metric ⓣ → AC-9
- [ ] `hu_eval_score_self_direction` beside the other scorers. Rubric tests:
      genuine-bounded-drive (high) vs reskinned-user-service / bound-violation (low).

## T8 — Full gate → AC-10
- [ ] Full suite green + 0 ASan; `/verify`; gate-symmetry + test-ref pass.
- [ ] Verify in an ISOLATED worktree (concurrent process active on branch).

## Dispatch
T1–T3 pure/parallelizable. T4–T6 share the runner + daemon → SEQUENTIAL. T7
independent. `/spec`-then-`/team` ONLY AFTER T0 threat-model sign-off. Sequence
the whole epic AFTER A1 (done) and A2 (soft dep).
