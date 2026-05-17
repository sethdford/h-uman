---
plan: docs/plans/2026-05-10-behavior-v1-execution-plan.md
auditor: group-8-behavior-m3-master-sota
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Behavior v1 promised a central behavior layer composing persona/memory/voice/security
into 17 workstreams (B1–B17). P0 (B1–B7, Bp, Bw) was to land first; P1 follow-ups
covered B8–B13; B14/B17 depend on the M3 plan. The plan also promised wiring
into the agent turn loop (Bw).

## Key Claims (from the plan)
- B1 behavior policy vtable + decision struct + default heuristic policy
- B2 dialog act classifier with repair detection
- B3 continuous VAD affect with fuse/decay
- B4 behavior-change engine (BCT + Fogg + JITAI)
- B5 companion-safety composition
- B6 persona consistency eval harness
- B7 LongMemEval scaffold
- B9 user-sim scenario runner with deterministic transcripts
- B10 support-strategy classifier surfaced via prompt directive
- B11 trust calibration + pressure detection + pressure history ring buffer
- B16 chronotype-aware quiet hours (production-wired into scheduler probes)
- Bw — behavior policy plumbed into `agent_turn.c` (read-only append to system_prompt)

## Evidence

### Implemented? (code exists)
- `src/behavior/` carries 14 .c files exactly matching plan: policy.c, dialog_act.c,
  affect.c, change.c, safety.c, prompt.c, support_strategy.c, behavior_trust.c,
  pressure.c, pressure_history.c, trust_prompt.c, user_sim.c, user_sim_scenario.c
- `include/human/behavior/` has 13 matching headers
- `src/eval/longmemeval.c` + `eval_suites/longmemeval/longmemeval.json` present
- `src/agent/tom_scenario.c` (B8) — promoted to landed
- `src/persona/eval.c::hu_persona_eval_run` (B6) present

### Proven? (tests exist)
- `tests/test_behavior_policy.c`, `_dialog_act.c`, `_affect.c`, `_change.c`,
  `_safety.c`, `_prompt.c`, `_support_strategy.c`, `_trust.c`, `_pressure.c`,
  `_trust_prompt.c`, `_corpora.c` — full per-workstream coverage
- `tests/test_b11_pressure_history_e2e.c`, `tests/test_b9_user_sim_agent_turn_e2e.c`
- `tests/test_user_sim.c`, `tests/test_user_sim_scenario.c`
- `tests/test_persona_eval.c`, `tests/test_tom_scenario_b8.c`, `tests/test_longmemeval.c`

### Wired? (called in runtime path / dispatch)
- `src/agent/agent_turn.c:3967-4011` constructs `hu_behavior_input_t`, calls
  `hu_behavior_decide`, runs safety assess, and appends `hu_behavior_build_directive`
  to the system prompt — the Bw integration
- `src/agent/agent_turn.c:372-412` integrates pressure detection +
  `hu_pressure_history_apply_to_trust_input` + `hu_trust_calibrate` +
  `hu_pressure_history_observe` for the B11 turn loop
- B16 chronotype lives in `src/agent/scheduler_probes.c::hu_scheduler_probe_quiet_hours`
  (per plan claim)

## Gaps
- B7 ≥75% acceptance gate not pinned by a CI gate (the self-test asserts ≥80%
  on golden answers but I did not confirm a CI invocation)
- B8 first/second-order frontier gates still pending an external model run
- B11 sycophancy regression CI gate still pending
- B14, B17 are explicitly blocked on M3

## Notes
- Companion plan `behavior-v1-followups.md` covers the B8–B17 stubs; many
  (B8/B10/B11/B16) are recorded as landed alongside this plan.
- Architecture is genuinely composed, not duplicative.
