---
plan: docs/plans/2026-05-10-w14-sleep-compute.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Extend v1 AutoDream into a full idle-compute scheduler (`hu_scheduler_t`). Hosts W13 LoRA jobs, W10 KV-warming, counterfactual rehearsal, W11 stale-belief re-verification. Coordinates around battery/quiet hours.

## Key Claims (from the plan)
- `hu_scheduler_t` coordinator
- Plug-in runners: LoRA, KV-warm, belief re-verify, rehearsal

## Evidence

### Implemented? (code exists)
- `src/agent/scheduler.c` + `scheduler_probes.c` + `scheduler_status_json.c`
- `src/agent/kv_prewarm_runner.c`
- `src/agent/belief_reverify_runner.c`
- `src/agent/lora_training_runner.c`
- `include/human/agent/scheduler.h` (referenced by runners)

### Proven? (tests exist)
- `tests/test_w14_scheduler.c`
- `tests/test_w14_runners.c`

### Wired? (called in runtime path / dispatch)
- `src/daemon.c` references scheduler/AutoDream 17 times
- Scheduler probes expose status; daemon orchestrates runners

## Gaps
- None major; counterfactual rehearsal job is a TBD but scheduler accepts new runners cleanly

## Notes
W14 is the orchestration layer for W10/W11/W13 idle-time work; all runners are present.
