---
plan: docs/plans/2026-05-11-init-05-verifier-driven-ttt.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: NONE
proven: NONE
wired: NONE
verdict: NOT_STARTED
confidence: HIGH
---

## Plan Summary
Verifier-Driven Test-Time Training: when the verifier panel flags a
low-fidelity response **and** a user correction turn arrives, perform a
tiny (1–10 step) on-device gradient update on the active LoRA adapter,
scoped to that conversation, with explicit rollback on subsequent
dissent. Critical-path position: 04 → 05 → 07 → 14.

## Key Claims (from the plan)
- Four trigger conditions for TTT activation
- Per-conversation LoRA gradient update path (1–10 steps)
- Rollback on subsequent user dissent
- Integration with `hu_learner_t` (CPU fallback) until Init 04 lands `load_adapter`
- New surface under `include/human/ml/` (TTT-specific) and `include/human/agent/response_verifier.h`

## Evidence

### Implemented? (code exists)
- NONE FOUND. Grep for `vds_ttt`, `hu_ttt`, `test_time_train`, `VDS-TTT` returned zero hits outside the plan.
- `include/human/ml/learner.h` exists (predecessor) but has no TTT/in-place-update entry point.
- `include/human/agent/response_verifier.h` referenced in plan frontmatter exists but contains no TTT trigger surface.

### Proven? (tests exist)
- NONE FOUND. No `tests/test_*ttt*.c`, no `tests/test_*test_time*.c`.

### Wired? (called in runtime path / dispatch)
- N/A — no code to wire.

## Gaps
- All four trigger conditions, the 1–10 step gradient update, and the rollback machinery are unimplemented.
- The plan explicitly says it depends on Init 04 closing Bridge B; that hasn't happened, so the only feasible Phase-0 ("plumbing-only proof against CPU `hu_learner_t`") is also unstarted.

## Notes
Plan honestly acknowledges its critical-path block. With Init 04 still a
stub, even the proof-of-plumbing variant requires net-new code that
hasn't started.
