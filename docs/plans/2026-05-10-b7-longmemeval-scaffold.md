---
title: "B7 — LongMemEval scaffold (memory v2 W16)"
created: 2026-05-10
status: deferred
parent: 2026-05-10-behavior-v1-execution-plan.md
related:
  - 2026-05-10-memory-v2-evidence-index.md
  - 2026-05-10-memory-v2-roadmap-overview.md
last_audit: 2026-05-25
---

# B7 — LongMemEval scaffold

LongMemEval is the May 2026 SOTA benchmark for long-term memory: 500 multi-session questions across five tasks (information extraction, multi-session reasoning, knowledge updates, temporal reasoning, abstention). h-uman has W16 evaluation infrastructure but no LongMemEval-shaped suite yet.

## Goal

A reproducible, offline harness that runs LongMemEval-shaped scenarios through h-uman's memory backends (SQLite, markdown, LRU, vector) and reports per-task pass rate plus a single aggregate score, gated by CI.

## Deliverables

1. `eval_suites/longmemeval/` directory with five JSON scenario packs (information extraction, multi-session reasoning, knowledge updates, temporal reasoning, abstention). Synthetic — no PII; no real corpus dependency.
2. `tools/eval_longmemeval/` runner (Python or `human eval run` invocation) that loads the packs, replays them through `hu_memory_t`, and prints per-task accuracy.
3. `tests/test_eval_longmemeval.c` — smoke test that the harness can load and score a tiny pack offline (≤5 questions per task).
4. `.github/workflows/eval.yml` extension to invoke the runner with the smoke pack.

## Why this matters

- Memory roadmap W16 specifies a behavioural eval gate; LongMemEval is the canonical shape.
- Without LongMemEval, neural memory and consolidation work has no public yardstick.
- Behaviour v1 (B1) also benefits: when memory contradicts a user claim, the policy must trigger DISCLOSE_UNCERTAINTY → PUSH_BACK. The "knowledge update" subtask exercises exactly that path.

## Success criteria

- CI runs the smoke pack offline in <60s.
- Aggregate score per backend printed; markdown vs SQLite gap visible.
- New work in `src/memory/` cannot regress aggregate score by more than 2 points without explicit reviewer override.

## Out of scope (this PR)

The full 500-question pack and the live-frontier comparison live behind `evaluation.yml` dispatch jobs (already in place). This plan tracks the smoke pack + runner only; the larger pack lands as part of W16 final.

## Risks

- Dataset drift if synthetic packs grow stale. Mitigation: tie pack version to `eval_suites/` directory hash, fail loudly on schema change.
- False sense of security from synthetic data. Mitigation: keep `evaluation.yml` live job alongside; document the gap in `2026-05-10-memory-v2-evidence-index.md`.
