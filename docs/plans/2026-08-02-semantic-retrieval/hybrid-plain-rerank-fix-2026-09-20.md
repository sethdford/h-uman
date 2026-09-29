# Production hybrid retrieval was collapsing to keyword recall (2026-09-20)

## What the benchmark had been measuring

`scripts/eval_memory_benchmarks.py`'s `hybrid_cli` column runs `human memory search
--hybrid`, which sets `hu_retrieval_options_t.reconstructive = true`. No daemon path
sets that flag: the only production caller of the retrieval engine is
`src/agent/memory_loader.c`, which asks for `HU_RETRIEVAL_HYBRID` with
`reconstructive` unset and `use_reranking = false`. The reconstructive mode is CLI-only.
Earlier notes calling `hybrid_cli` "the production hybrid path" were wrong.

The harness now has a `hybrid_plain` column (`search --hybrid --plain`) that makes the
loader's exact call.

## The defect

`hu_hybrid_retrieve`'s plain merge ran `hu_rerank_cross_encoder` unconditionally after
the RRF merge. That "cross-encoder" is a query-word-overlap fraction; sorting the fused
pool by it before the cut to `limit` discards the semantic ranking and evicts every
semantic-only hit. `use_reranking` was honoured only by the engine's later MMR pass.

Measured on the Gemma index (60 + 60 questions, seed 3, same protocol as the embedder
comparison):

| column | LongMemEval-S R@5 | LoCoMo-10 R@10 |
|---|---|---|
| keyword only | 0.883 | 0.650 |
| semantic only | 1.000 | 0.833 |
| plain RRF of the two lists (Python) | 1.000 | 0.850 |
| reconstructive CLI mode | 0.917 | 0.600 |
| **production call, before** | **0.817** | **0.650** |
| **production call, after** | **1.000** | **0.783** |

Before the fix the production call scored exactly the keyword-only number on LoCoMo.

Fix: the plain path runs the overlap rerank only when `opts->use_reranking` is set.
Pinned by `test_plain_hybrid_without_reranking_keeps_semantic_only_hit`
(`tests/test_hybrid_reconstructive.c`): a class-2 query, one semantic-only hit with no
query words, two keyword hits under `experience:` keys (never embedded, so no fusion
boost), limit 2 — RRF keeps the semantic hit, the rerank evicted it.

Evidence: `memory-benchmarks-hybrid-plain-gemma-2026-09-20.json` (before),
`memory-benchmarks-hybrid-plain-fixed-gemma-2026-09-20.json` (after).

## Left open

- ~~LoCoMo: C plain path 0.783 vs the harness's Python RRF 0.850.~~ Resolved the same
  day: the merge and the semantic leg were identical; the keyword leg was
  `hu_keyword_retrieve` (matched-word fraction) instead of the backend's BM25 `recall`.
  See `hybrid-plain-keyword-leg-2026-09-20.md` (production call now 0.850).
- Deploying the fix changes what reaches the prompt on every daemon turn. The next
  weekly semantic gate is the downstream measurement.
