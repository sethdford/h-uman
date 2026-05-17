---
plan: docs/plans/2026-05-11-init-07-thinkprm-verifier.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: NONE
verdict: PARTIAL
confidence: MEDIUM
---

## Plan Summary
Replace the prompt-based critic chain on the response path with a
small trained Process Reward Model (PRM) calibrated on h-uman's own
DPO data, running on-device behind `HU_VERIFIER_TRAINED=1`. New PRM
surface under `include/human/agent/process_reward.h`.

## Key Claims (from the plan)
- `HU_VERIFIER_TRAINED` env or compile flag toggles the trained verifier
- PRM trained from `hu_dpo_collector` data
- Surface in `include/human/agent/process_reward.h`
- Replaces prompt-based critic chain on the agent response path
- Calibration tests against existing prompt-based critic

## Evidence

### Implemented? (code exists)
- `include/human/agent/process_reward.h` exists (47 LOC) — declares `hu_prm_config_t`, `hu_prm_config_default()`.
- `src/agent/process_reward.c` exists (256 LOC) — implements `hu_prm_config_default()`, helper predicates (`prm_contains`, `has_digit`, `has_code_pattern`) — pure heuristic scoring, no trained model load, no provider hook.
- Grep for `HU_VERIFIER_TRAINED`, `ThinkPRM`, `thinkprm` returned zero hits outside the plan doc — the trained-mode toggle is not wired.

### Proven? (tests exist)
- `tests/test_process_reward.c` exists. Other tests reference PRM concepts: `tests/test_sota_e2e.c`, `tests/test_sota_adversarial.c`, `tests/test_agi_frontiers.c`, and a fuzzer `fuzz/fuzz_prm.c`.
- These cover the heuristic scoring path only; no test exercises a trained PRM.

### Wired? (called in runtime path / dispatch)
- Grep for `hu_process_reward` / `process_reward_step` / `process_reward_score` across `src/agent/`, `src/cognition/` returned only `src/agent/process_reward.c` itself — i.e., **no call sites in the runtime agent loop**. The heuristic PRM exists as compiled code but is not invoked on the response path.

## Gaps
- No trained-model loader (the plan's central deliverable).
- No `HU_VERIFIER_TRAINED` gate.
- No replacement of the prompt-based critic chain (the agent turn does not call `hu_prm_*`).
- No training pipeline from `hu_dpo_collector` to a PRM artifact.

## Notes
What exists looks like an earlier "heuristic PRM" surface that predates
this plan; the plan would replace the heuristic scorer with a trained
small model. The current code is orphan-ish (not invoked) and unrelated
to the plan's trained-PRM thesis.
