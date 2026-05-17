---
plan: docs/plans/2026-05-10-w6-eval-memrl-redteam.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Full LoCoMo + LongMemEval harness; MemRL write-policy LoRA from W3 case outcomes; memory-poisoning red-team (MINJA + Echoleak + Unit 42).

## Key Claims (from the plan)
- LoCoMo full benchmark
- MemRL LoRA write-policy
- Continuous red-team

## Evidence

### Implemented? (code exists)
- `src/evaluation/evaluation_locomo.c`, `evaluation_locomo_facade.c`, `evaluation_longmemeval.c`
- `src/evaluation/evaluation_minja.c` — MINJA poisoning attacks (`MINJA_ATTACKS[]` array)
- `src/evaluation/evaluation_memoryagentbench.c`, `evaluation_dmr.c`
- `src/ml/` — LoRA + DPO modules (lora.c, dpo.c) plus `src/agent/lora_training_runner.c`

### Proven? (tests exist)
- `tests/test_w6_e2e_adversarial.c` — composed adversarial scenario suite
- `tests/test_w16_evaluation.c`, `test_w16_eval_cli.c`

### Wired? (called in runtime path / dispatch)
- `human eval` CLI invokes evaluation vtable; nightly evaluation.yml workflow
- MINJA scenarios run in CI via `redteam-eval-fleet.sh` per execution plan

## Gaps
- MemRL write-policy LoRA loop: lora_training_runner.c exists but the closed loop to "reinforce on case-based outcomes" is more advanced wiring tracked under W13

## Notes
W6 + W16 essentially overlap; v2 evolved the W6 plan into W16 as the eval suite layer of the v2 stack.
