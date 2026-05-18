---
plan: docs/plans/2026-05-10-b7-longmemeval-scaffold.md
auditor: group-7-memory
audited_at: 2026-05-17
implemented: PARTIAL
proven: PARTIAL
wired: PARTIAL
verdict: PARTIAL
confidence: MEDIUM
---

## Plan Summary
Stand up an offline LongMemEval-shaped harness (5 task categories), runner under `tools/eval_longmemeval/`, smoke test, and CI wiring for the W16 memory evaluation gate.

## Key Claims (from the plan)
- `eval_suites/longmemeval/` directory with five JSON scenario packs
- `tools/eval_longmemeval/` runner
- `tests/test_eval_longmemeval.c` smoke test
- `.github/workflows/eval.yml` extension

## Evidence

### Implemented? (code exists)
- `eval_suites/longmemeval/longmemeval.json` + `README.md` — single example pack present (not five separate task packs)
- `src/evaluation/evaluation_longmemeval.c` — W16 LongMemEval backend exists with five-task scaffolding
- No `tools/eval_longmemeval/` runner directory found

### Proven? (tests exist)
- `tests/test_longmemeval.c` exists (covers loader + scoring)
- `tests/test_w16_eval_cli.c` covers W16 CLI

### Wired? (called in runtime path / dispatch)
- Backend registered via `hu_evaluation_longmemeval` (W16 vtable) — invokable via `human eval` CLI
- `eval.yml` workflow runs eval_suites/ validation per execution plan

## Gaps
- Only one example pack, not five per-task JSON packs as plan promised
- `tools/eval_longmemeval/` Python runner not present (likely subsumed by C backend + `human eval run`)
- Smoke test in CI present (test_longmemeval.c) but not a dedicated `eval.yml` LongMemEval invocation step

## Notes
The plan's runner shape changed: instead of a Python tool, the harness is the C-side `hu_evaluation_longmemeval` backend exposed via `human eval`. Functionally equivalent, but plan deliverables 2 and 4 are partially superseded by W16's evaluation vtable.
