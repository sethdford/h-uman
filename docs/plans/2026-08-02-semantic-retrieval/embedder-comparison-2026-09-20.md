# Embedder comparison — nomic modernbert-embed-base (current) vs EmbeddingGemma-300m, 2026-09-20

Same harness (`scripts/eval_memory_benchmarks.py`, production binary `~/.local/bin/human-daemon`
b57d3973d, fresh memory.db per question, FTS5 + sqlite-vec via the embeddings endpoint), same
60 LongMemEval-S and 60 LoCoMo-10 questions, same day. Only the embedder differs; the
keyword column is identical in both arms, which is the check that nothing else moved.

Arms: nomic via `scripts/embed_server.py` on :8749 (the model :8741 serves today);
EmbeddingGemma-300m (mlx-community 8-bit, 768-d) via two instances — documents indexed with
the `title: none | text: ` prefix (:8747, `HU_SEMANTIC_EMBED_URL_INDEX`), queries with
`task: search result | query: ` (:8746) — the asymmetric encoding the model is trained with.

| metric | LongMemEval-S R@5 nomic → gemma | LoCoMo-10 R@10 nomic → gemma |
|---|---|---|
| keyword only (control) | 0.883 → 0.883 | 0.650 → 0.650 |
| semantic only | 0.983 → 1.000 | 0.767 → 0.833 |
| harness hybrid (RRF) | 0.967 → 1.000 | 0.783 → 0.850 |
| production hybrid path (`--hybrid`) | 0.817 → 0.917 | 0.533 → 0.600 |

Gemma wins every non-control cell: +6 questions of 60 on the production path for
LongMemEval, +4 of 60 on LoCoMo. Files: `memory-benchmarks-embedder-{nomic,gemma}-2026-09-20.json`.

Not a production change. Switching requires: (1) the C embed calls to mark index vs query so
:8741 can apply the two prefixes (today both hit one endpoint with raw text), (2) a full
`memory reindex` (vectors are not comparable across models), (3) a paired SHADOW/LIVE
semantic-gate run on the new index before the flip, (4) a :8741 restart — Seth's call.
