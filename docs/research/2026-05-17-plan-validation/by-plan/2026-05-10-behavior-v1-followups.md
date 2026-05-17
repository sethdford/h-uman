---
plan: docs/plans/2026-05-10-behavior-v1-followups.md
auditor: group-8-behavior-m3-master-sota
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: HIGH
---

## Plan Summary
Compact follow-up stubs for B8–B17 from the Behavior v1 execution plan. Each
stub captured scope, success criteria, and a first commit. Three (B8, B10, B11,
B16) were already promoted to "landed" status in the same document.

## Key Claims (from the plan)
- B8 ToM synthetic pack — landed (synthesis, gold matcher, response-array scorer, world-model bridge)
- B9 user-sim vtable + 50-scenario regression — partially scoped
- B10 support-strategy classifier surfaced via prompt directive — landed
- B11 trust calibration + pressure detector + pressure history + trust prompt — landed
- B12 audio prosody stub — landed (neutral VAD, high uncertainty)
- B13 repair eval pack — landed via `eval_suites/repair/` + `tests/test_behavior_corpora.c`
- B14 LoRA persona control — blocked on M3
- B15 cultural pragmatics overlay — landed (explicit opt-in only)
- B16 chronotype-aligned JITAI — production-wired
- B17 on-device frontier behavior — depends on M3

## Evidence

### Implemented? (code exists)
- `src/agent/tom_scenario.c` + `include/human/agent/tom_scenario.h` (B8)
- `eval_suites/tom/tom_synthetic.json` (B8)
- `src/behavior/support_strategy.c` (B10)
- `src/behavior/behavior_trust.c`, `pressure.c`, `pressure_history.c`,
  `trust_prompt.c` (B11)
- `src/behavior/affect.c::hu_affect_estimate_audio` (B12, stub)
- `eval_suites/repair/` (B13)
- `src/behavior/user_sim*.c` (B9 — present, exceeds stub status)

### Proven? (tests exist)
- `tests/test_tom_scenario_b8.c` (B8)
- `tests/test_behavior_support_strategy.c` (B10)
- `tests/test_behavior_trust.c`, `_pressure.c`, `_trust_prompt.c`,
  `tests/test_b11_pressure_history_e2e.c` (B11)
- `tests/test_behavior_affect.c` (B12)
- `tests/test_behavior_corpora.c` (B13)
- `tests/test_user_sim.c`, `tests/test_user_sim_scenario.c`,
  `tests/test_b9_user_sim_agent_turn_e2e.c` (B9)
- `tests/test_w14_scheduler.c` (B16 quiet-hours per plan claim)

### Wired? (called in runtime path / dispatch)
- B11 pipeline (pressure → trust → directive) wired in
  `src/agent/agent_turn.c:372-412`
- B10 directive appended via `hu_behavior_build_directive` which is itself
  called at `agent_turn.c:4009`
- B16 quiet hours consumed by `hu_scheduler_probe_quiet_hours` (gates proactive
  messages from `src/agent/scheduler.c`)
- B14, B17 explicitly not wired — they wait on M3

## Gaps
- Sycophancy regression CI gate (B11) not pinned to a green run
- B8 frontier-model first/second-order gates not landed
- B9 50-scenario CI replay not yet wired

## Notes
- This is a tracking stub doc; per-stub statuses are honest and align with
  filesystem/grep evidence. PARTIAL reflects the still-open eval gates and
  M3-dependent items, not any drift in the landed work.
