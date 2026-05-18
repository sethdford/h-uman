---
plan: docs/plans/2026-05-11-full-sota-rl-improvement-loop.md
auditor: group-9-adversarial-rl
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: PARTIAL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
Umbrella implementation plan sequencing Phases 0–6 of the RL & neural improvement loop on top of Track D Phase 1 infrastructure. Each phase has its own per-phase plan authored just-in-time. Ship contract = the 14-item DoD reproduced verbatim from the design spec §9.

## Key Claims (from the plan)
- Claim 1: 6 phases sequenced; each phase ends with a `rl-sota-phase-<N>-complete` git tag.
- Claim 2: All 14 Ship Contract items reach PASS or PASS_WITH_NOTES.
- Claim 3: Status table at line 159 is the single source of truth for phase verdicts.

## Evidence

### Implemented? (code exists)
- Phase 0 tag `rl-sota-phase-0-complete`; Phase 1 tag `rl-sota-phase-1-complete`; Phase 2 tag `rl-sota-phase-2-complete` (referenced in commit 75a3687a); Phase 3 tag (commits 92327f7a / 29a1de90 / 4a1f25b1 land KTO + RM); Phase 6 tag `rl-sota-phase-6-complete` at commit `3a17a528`.
- All RL trainer source files present on origin/main (see design-spec audit).
- Phase D closure commit `ebb56bf3` ("close Phase D carry-forwards CF-1..CF-7") merged.

### Proven? (tests exist)
- 10330/10332 PASS at tag `rl-sota-phase-6-complete` (per ship-contract doc).
- E2E suite `tests/test_e2e_rl_loop.c` present + 4 closed-loop tests.
- Per-method tests: dpo_real, kto, grpo, reward_model all have dedicated test files.

### Wired? (called in runtime path / dispatch)
- `human ml dpo-train`, `human ml kto-train`, `human ml grpo-train`, `human ml rm-train`, `human eval competitive`, `human demo rl-closed-loop` are all real CLI subcommands.
- Daemon reaction poll + lora_training_runner wired; persona_rollout, eval_gate, competitive_harness consumed by `cli_eval.c`.
- **Gap:** No `rl_nightly.yml` GitHub workflow auto-runs RL training. `eval.yml` runs weekly; `validate-rl-sota.sh` is the on-demand validator.

## Gaps
- Phase 5/6 status is closed but with PASS_WITH_NOTES on DoD-3 (18/20 sanity), DoD-8 (eval gate via separate suite), DoD-10 (proofs with reasonable but not strict adapter id), DoD-14 (Apple FM/Gemini Nano return `HU_ERR_NOT_SUPPORTED`).
- Nightly automation is the one remaining wiring gap; everything else is shipped.

## Notes
The umbrella plan honestly captures PASS_WITH_NOTES — auditors caught inflations in early drafts of the ship-contract doc and the verdicts are now corrected. The closure was tracked through `010763ef`, `2d842c69`, `481ef4d4`, `e8c90301` close-out commits.
