---
plan: docs/plans/2026-05-10-w16-evaluation-suite.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: FULL
proven: FULL
wired: FULL
verdict: SHIPPED
confidence: HIGH
---

## Plan Summary
`hu_evaluation_t` vtable with six backends: LoCoMo, LongMemEval, DMR, MINJA, MemoryAgentBench, frontier-compare. Nightly GitHub Actions, fail-on-regression.

## Key Claims (from the plan)
- Six benchmark backends as vtable
- Nightly workflow `evaluation.yml`
- Regression gate

## Evidence

### Implemented? (code exists)
- `include/human/evaluation/evaluation.h`
- `src/evaluation/evaluation.c` — vtable dispatcher
- `src/evaluation/evaluation_locomo.c` + `evaluation_locomo_facade.c`
- `src/evaluation/evaluation_longmemeval.c`
- `src/evaluation/evaluation_dmr.c`
- `src/evaluation/evaluation_minja.c` (with `MINJA_ATTACKS[]` corpus)
- `src/evaluation/evaluation_memoryagentbench.c`
- `src/evaluation/evaluation_frontier_compare.c`
- `src/evaluation/evaluation_regression.c`
- `src/evaluation/evaluation_facade_recall.c`
- `src/evaluation/evaluation_baseline.c`
- `src/evaluation/evaluation_dataset_loader.[ch]`
- `src/evaluation/evaluation_legacy_bridge.c`

### Proven? (tests exist)
- `tests/test_w16_evaluation.c`
- `tests/test_w16_eval_cli.c`
- `tests/test_longmemeval.c`
- `tests/test_eval_benchmarks.c`, `test_eval_history.c`, `test_eval_judge.c`, `test_eval_runner.c`

### Wired? (called in runtime path / dispatch)
- `human eval` CLI dispatches into `hu_evaluation_t` vtable
- `.github/workflows/evaluation.yml` nightly + dispatch
- `.github/workflows/eval.yml` PR-scoped + offline red-team
- ci.yml runs `human eval validate` and `human eval check-regression`

## Gaps
- All 6 backends present + a few extras (frontier compare, regression, baseline)

## Notes
This is the most complete W-stream. Vtable + 6 backends + 3 CI workflows all real.
