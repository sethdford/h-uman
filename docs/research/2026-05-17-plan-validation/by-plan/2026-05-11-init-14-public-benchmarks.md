---
plan: docs/plans/2026-05-11-init-14-public-benchmarks.md
auditor: group-10-init-series
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: MEDIUM
---

## Plan Summary
Wire LongMemEval, LoCoMo, KnowU-Bench, EMPA, and ProAgentBench into
`human_tests` as deterministic smoke gates, plus a separate
longitudinal `human eval --full` path. Publish methodology + numbers
with h-uman vs Gemini Personal Intelligence vs Claude Cowork
side-by-side. Binary-budget delta ≤ 8 KB CLI plumbing only.

## Key Claims (from the plan)
- Five benchmarks integrated: LongMemEval, LoCoMo, KnowU-Bench, EMPA, ProAgentBench
- Deterministic smoke gates in `human_tests`
- `human eval --full` longitudinal path
- Published methodology + side-by-side numbers
- ≤8 KB binary delta

## Evidence

### Implemented? (code exists)
- LongMemEval: `include/human/eval/longmemeval.h`, `src/eval/longmemeval.c`, `src/evaluation/evaluation_longmemeval.c`. Wired into the CLI registry: `src/cli_evaluation.c` line 41–42 registers `"longmemeval"`.
- LoCoMo: `src/evaluation/evaluation_locomo.c`, `src/evaluation/evaluation_locomo_facade.c`. CLI registers `"locomo"` (L39–40) and `"locomo-facade"` (L56–58).
- MemoryAgentBench: `src/evaluation/evaluation_memoryagentbench.c`, registered at L47–48 (CLI label says `"stub"`).
- KnowU-Bench, EMPA, ProAgentBench: **not found**. No matching files, no CLI registry entries.
- `src/cli_evaluation.c` is the `human evaluation ...` subcommand surface; `src/main.c` line 492 registers it.

### Proven? (tests exist)
- `tests/test_longmemeval.c`, `tests/test_w16_evaluation.c`, `tests/test_w16_eval_cli.c` exist.
- No tests for KnowU-Bench, EMPA, or ProAgentBench.

### Wired? (called in runtime path / dispatch)
- The three landed benchmarks are wired via the CLI subcommand registry in `cli_evaluation.c` and reachable via `human evaluation longmemeval|locomo|memoryagentbench`.
- No competitive side-by-side comparison harness (Gemini PI / Claude Cowork) visible — the "publish numbers" deliverable was not located.

## Gaps
- 2/5 benchmarks (KnowU-Bench, EMPA, ProAgentBench) not wired.
- Side-by-side competitive harness (the plan's narrative-win deliverable) not present.
- Published methodology document and reproducibility script were not found under `docs/`, `benchmarks/`, or `scripts/`.

## Notes
LongMemEval/LoCoMo predate this plan (the related links cite
`b7-longmemeval-scaffold` and `w16-evaluation-suite`). This plan would
extend them by 3 benchmarks + the competitive harness. The two
existing benchmarks remain wired; the plan's net-new deliverables did
not land.
