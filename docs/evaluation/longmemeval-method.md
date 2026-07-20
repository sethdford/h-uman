---
title: LongMemEval evaluation method notes
date: 2026-07-19
status: active
---

# LongMemEval method notes (Wave B)

## What is published

`docs/evaluation/baseline.json` locks category scores under suite `longmemeval`:

| Metric | Score | n |
|--------|-------|---|
| `category_temporal` | 1.0 | 121 |
| `category_multi_hop` | 1.0 | 75 |
| `category_single_hop` | 1.0 | 142 |
| `category_knowledge_update` | 1.0 | 59 |
| `category_abstention` | 0.0 | **0** (no samples in locked run) |
| `real_corpus` | 1.0 | 397 |

These scores come from the in-repo W16 / keyword self-test path (`src/evaluation/evaluation_longmemeval.c`, `src/eval/longmemeval.c`), optionally against a fetched HuggingFace pack via `scripts/fetch-evaluation-datasets.sh longmemeval`.

## How to reproduce

```bash
bash scripts/fetch-evaluation-datasets.sh longmemeval
./build/human_tests --suite=LongMemEval   # offline scorer unit coverage
# Full suite runner when dataset present:
./build/human evaluation --w16 longmemeval
```

Example pack: `eval_suites/longmemeval/longmemeval.json`.

## Scope / limits (honest)

- This is **not** yet a full end-to-end “retrieve from live SQLite + answer” LongMemEval leaderboard run.
- `category_abstention` has **zero** samples in the locked baseline — do not treat abstention as measured.
- Wave B publishes these numbers as the **program gate snapshot**. Improving retrieval quality is follow-on work; restore any SOTA memory claim in `docs/SOTA_BENCHMARK.md` only after a stronger measured bar.

## Regression

Treat locked category scores as floors for CI regression once the live W16 job is enabled on a machine with the corpus; until then unit tests cover the scorer.
