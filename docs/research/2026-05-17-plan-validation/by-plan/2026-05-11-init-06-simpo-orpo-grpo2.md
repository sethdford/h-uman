---
plan: docs/plans/2026-05-11-init-06-simpo-orpo-grpo2.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: NONE
proven: NONE
wired: NONE
verdict: NOT_STARTED
confidence: HIGH
---

## Plan Summary
Move beyond DPO-only preference optimization. Add three new factories —
SimPO (reference-free), ORPO (preference + SFT in one pass), GRPO-2
(cheap group-relative variant) — each behind a single
`hu_rl_trainer_t` vtable. Existing `hu_dpo_judge_step` stays as
baseline. Single CLI surface `human ml rl-train --algo=...`.

## Key Claims (from the plan)
- New vtable `hu_rl_trainer_t` in `include/human/ml/`
- Three loss-head implementations (SimPO, ORPO, GRPO-2) registered as factories
- CLI `human ml rl-train` with `--algo` selector
- Deterministic golden tests per loss head
- Builds on existing `include/human/ml/dpo.h`, `lora.h`, `optimizer.h`

## Evidence

### Implemented? (code exists)
- NONE FOUND. Grep for `simpo`, `orpo`, `grpo`, `hu_rl_trainer`, `SimPO`, `ORPO`, `GRPO` in `src/` and `include/` returned zero matches. (The hits in `tests/test_persona*.c` and `vendor/sqlite3/sqlite3.c` are unrelated — they contain incidental substrings like "isOrphan" or persona examples.)
- `include/human/ml/dpo.h` still exposes only `hu_dpo_collector_*` for offline pair collection — no `hu_rl_trainer_t` vtable.
- `include/human/ml/cli.h` and `src/ml/cli.c` don't contain an `rl-train` subcommand.
- No `src/ml/simpo*.c`, `src/ml/orpo*.c`, `src/ml/grpo*.c`.

### Proven? (tests exist)
- NONE FOUND. No `tests/test_simpo*.c`, no `tests/test_orpo*.c`, no `tests/test_grpo*.c`.

### Wired? (called in runtime path / dispatch)
- N/A — no code to wire.

## Gaps
- Vtable absent. No `hu_rl_trainer_t` typedef anywhere.
- Three loss heads unimplemented.
- CLI factory absent.

## Notes
Plan status header is "design". This is one of the cleanest "design only,
zero code" findings in the Init series.
