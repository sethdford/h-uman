---
title: LoCoMo evaluation method notes
date: 2026-07-19
status: active
---

# LoCoMo method notes (Wave B)

## Suites in `docs/evaluation/baseline.json`

| Suite | Metric | Score (locked) | n | What it measures |
|-------|--------|----------------|---|------------------|
| `locomo` | `precision_at_1` | **0.057717** | 1542 | W16 LoCoMo backend against the real corpus loader |
| `locomo-facade` | `precision_at_1` | 0.731517 | 1542 | Production-stack facade (graph/planner path) — **not** the Wave B honesty bar |

Wave B treats **`locomo` / P@1 ≈ 0.058** as the documented floor until a measured improvement is published with the same method.

## How to reproduce

```bash
# Fetch corpus (not committed; lands under ~/.human/eval-datasets/)
bash scripts/fetch-evaluation-datasets.sh locomo

# Run W16 LoCoMo via CLI (requires built binary + dataset)
./build/human evaluation --w16 locomo
```

Corpus path: `~/.human/eval-datasets/locomo.json` (see `hu_eval_locomo_load`).

## Regression gate

`src/evaluation/evaluation_regression.c` allows at most a **0.02** absolute drop on `locomo` / `precision_at_1` vs the locked baseline.

## Honesty

Do not claim memory SOTA from `locomo-facade` alone. Facade and raw LoCoMo are different pipelines; only the `locomo` suite is the Wave B external-style bar cited in `docs/SOTA_BENCHMARK.md`.
