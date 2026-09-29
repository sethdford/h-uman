# The production hybrid's keyword leg was the wrong keyword search (2026-09-20)

Follow-up to `hybrid-plain-rerank-fix-2026-09-20.md`, which left open: on LoCoMo-10 the
production call (`search --hybrid --plain`, the memory loader's `hu_hybrid_retrieve` with
`reconstructive=false`, `use_reranking=false`) scored R@10 0.783 while the harness's own
Python RRF of the `search` and `search --semantic` lists scored 0.850.

## Method

A scratch script re-ran the harness's exact LoCoMo selection (seed 3, 60 questions,
EmbeddingGemma on :8741) with a temporary env-gated stderr print inside
`hu_hybrid_retrieve` that emitted the keyword leg, the semantic leg and the RRF-merged
pool the C path actually built, then diffed them against the CLI lists the harness fuses.
Evidence: `hybrid-plain-keyword-leg-dump-2026-09-20.json` (summary, the six diverging
questions with all lists, and per-question hit flags).

## What the dump showed

| check | result |
|---|---|
| C plain output == Python RRF over the C path's *own* two legs | 60/60 same set, 60/60 same order |
| C semantic leg == CLI `search --semantic` list | 60/60 identical |
| C keyword leg == CLI `search` list | 0/60 identical |
| evidence inside the top-10 keyword list | CLI (FTS5 BM25): 39/60; C leg: 27/60 |
| C keyword leg cut at a tied score | 57/60 |

So `hu_rerank_rrf` was not the cause (it is an exact RRF, k=60, and even the tie order
matched), the leg limits were not the cause (both legs are 10 long), and namespace filtering
was not the cause. The whole gap was one thing: **the hybrid's keyword leg was
`hu_keyword_retrieve`, not the backend's `recall`.**

`hu_keyword_retrieve` lists every row and scores it by the fraction of whitespace-split
query words it contains, with no term weighting and no length normalisation. On a real
corpus most candidates tie (a 7-word question gives every row with three of the words
exactly 0.4286), and the leg fills in `list` order, not by relevance. The CLI's `search`,
which the harness fuses, is the sqlite engine's FTS5 BM25 `recall` (plus its LIKE
fallback and MAGMA graph re-rank), which breaks those ties by rarity and document length.
RRF reads ranks only, so a leg whose ranks are arbitrary contributes noise, and fusing
noise with a good semantic list pushed the correct hit below rank 10 in five questions
(and above it in one).

## Fix

`hybrid_keyword_leg()` in `src/memory/retrieval/hybrid.c`: the non-reconstructive path
now takes its keyword leg from `backend->vtable->recall` (the same list `human memory
search <q>` prints), with rank-derived scores so downstream consumers that read scores
(the no-vector fallback, the engine's temporal and MMR passes) still see a positive,
monotone value. It falls back to `hu_keyword_retrieve` when the backend has no recall,
recall fails or returns nothing, or the caller set `min_score` (recall has no threshold to
honour).

Reconstructive mode (Contract C2, CLI-only) keeps the fraction leg on purpose: its
scene-select stage and its ablation tests were built around that leg's tied-score
truncation (the fixtures say so), and its `hybrid_cli` benchmark column is a separate
experiment. Switching that leg is its own measured change.

Pinned by `test_plain_hybrid_keyword_leg_ranks_by_backend_recall_not_word_fraction`
(`tests/test_hybrid_reconstructive.c`): five `experience:` rows (never embedded, so the
semantic leg is empty), a 123-word row carrying all three query words vs a two-word row
that is nothing but the query, limit 1. The fraction scorer returns the padded row (it
matched 3/3 words); BM25 returns the short one (-1.04 vs -0.78 on the same rows). The test
failed against the unfixed code and passes with the fix.

One fixture moved with it: `test_hybrid_retrieve_register_gate_live_suppresses_casual_turn`
assumed the query "xyz" could not match keyword search. That held only for the whole-word
scorer; the backend's LIKE fallback matches it as a substring of every row. The query is now
"xq" (same stub-embedder class, not a substring of any row), the intent unchanged.

## Re-measured (same protocol: seed 3, 60 + 60 questions, Gemma index, :8741)

| column | LoCoMo-10 R@10 before | after |
|---|---|---|
| keyword only (CLI) | 0.650 | 0.650 |
| semantic only | 0.833 | 0.833 |
| plain RRF of the two lists (Python) | 0.850 | 0.850 |
| reconstructive CLI mode | 0.633 | 0.633 |
| **production call** | **0.783** | **0.850** |

Every category matches the Python RRF exactly (1: 0.75, 2: 0.917, 3: 0.667, 4: 0.952,
5: 0.75).

| column | LongMemEval-S R@5 before | after |
|---|---|---|
| keyword only (CLI) | 0.883 | 0.883 |
| semantic only | 1.000 | 1.000 |
| plain RRF of the two lists (Python) | 1.000 | 1.000 |
| reconstructive CLI mode | 0.933 | 0.933 |
| **production call** | **1.000** | **1.000** |

"Before" is `memory-benchmarks-hybrid-plain-fixed-gemma-2026-09-20.json` (the run after the
rerank fix, the immediately preceding state of this path).

Files: `memory-benchmarks-hybrid-plain-keyword-leg-gemma-2026-09-20.json` (LoCoMo),
`memory-benchmarks-hybrid-plain-keyword-leg-gemma-longmemeval-2026-09-20.json`
(LongMemEval), `hybrid-plain-keyword-leg-dump-2026-09-20.json` (the dumped lists).

## Left open

- `HU_RETRIEVAL_KEYWORD` mode (the engine's keyword-only mode) and the reconstructive CLI
  mode still use the fraction scorer. Neither is on the daemon's turn path; each is a
  separate measured switch.
- Deploying this changes which rows the loader fuses on every daemon turn. The weekly
  semantic gate is the downstream measurement.
