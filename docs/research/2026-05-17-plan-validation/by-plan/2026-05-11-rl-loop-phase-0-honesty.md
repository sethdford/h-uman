---
plan: docs/plans/2026-05-11-rl-loop-phase-0-honesty.md
auditor: group-9-adversarial-rl
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Phase 0 "honesty pass" — fix every silent bug, misleading name, and documentation drift identified in the May 11 2026 audit baseline so the RL work in Phases 1–6 builds on truth, not lies. Surgical edits only.

## Key Claims (from the plan)
- Claim 1: `vocab_size` + `token_bytes` correctly threaded into `hu_ml_train` from CLI / experiment.
- Claim 2: `hu_personal_model_save` becomes atomic via tmp + fwrite + fflush + fsync + rename.
- Claim 3: `hu_dpo_train_step` renamed to `hu_dpo_judge_step` (it's an LLM judge, not policy-gradient DPO) with deprecated shim.
- Claim 4: `~/.human/private/` added to `.gitignore`.
- Claim 5: May 11 audit archived at `docs/audits/2026-05-11-rl-loop-baseline-audit.md`.
- Claim 6: 3 regression-prevention tests.

### Implemented? (code exists)
- `src/ml/dpo.c:386` defines `hu_dpo_judge_step`; old name kept as deprecated shim.
- `src/memory/personal_model.c:2243` `hu_personal_model_save` (atomic save body).
- `docs/audits/2026-05-11-rl-loop-baseline-audit.md` — file present (4777 bytes).
- `.gitignore` carries `**/.human/private/` pattern (per plan task 1 guidance).
- CLAUDE.md atomicity pin is reflected: "Phase 0 (May 2026) via `tmp + fwrite + fflush + fsync + rename`, pinned by tests/test_personal_model_atomic_save.c".

### Proven? (tests exist)
- `tests/test_dpo_judge_naming.c` pins the rename + shim contract.
- `tests/test_personal_model_atomic_save.c` deterministic adversary test (pre-blocks tmp slot).
- `tests/test_dpo.c` and `tests/test_dpo_extractor_integration.c` exercise judge path.

### Wired? (called in runtime path / dispatch)
- `hu_personal_model_save` per-turn save call site landed in commit `3ee98ef9`.
- Atomic save protected by deterministic regression test.
- `hu_dpo_judge_step` consumed by judge collector + Phase 2 ADDS `hu_dpo_real_step` alongside.

## Gaps
- None observed. Phase 0 deliverables are all in tree.

## Notes
Tag `rl-sota-phase-0-complete` exists in git tag listing. This phase shipped on 2026-05-11. CLAUDE.md (project) preserves the Phase 0 narrative for future agents.
