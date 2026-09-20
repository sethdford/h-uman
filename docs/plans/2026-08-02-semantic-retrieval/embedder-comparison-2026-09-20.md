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

## Switch executed and gated (2026-09-20)

Production moved to EmbeddingGemma on Seth's instruction: `:8741` serves
`mlx-community/embeddinggemma-300m-8bit` for `/v1/embeddings` (query/document
prefixes chosen by the request's `input_type`), the daemon at `8afe01ad9` marks
every embed request, and the live index was rebuilt with `human memory reindex
--full` (720 rows; the previous 1547 held 827 ids of already-pruned memories and
the `experience:` scaffold rows the content filter excludes). The first rebuild
attempt was a silent no-op — the CLI never read a trailing `--full` — fixed in
`732f79ab9`.

Paired semantic-recall gate on the Gemma index (`eval_semantic_live_gate.py`,
40 contexts, 39 paired, coverage 1.0, arms shadow=no recall / live=recall,
`~/.human/logs/semantic-gate-gemma-embed-2026-09-20.json`):

| run | index | shadow (no recall) | live (recall) | verdict |
|---|---|---|---|---|
| 2026-09-10 | nomic | 0.866 / EI 3.795 | 0.860 / EI 3.744 | PROMOTE |
| 2026-09-17 | nomic | 0.866 / EI 3.795 | 0.878 / EI 3.923 | PROMOTE |
| 2026-09-20 | **Gemma** | 0.901 / EI 4.128 | 0.878 / EI 3.923 | **HOLD** |

Reading it honestly: the recall arm — the only arm the embedder touches — scored
the same aggregate as the nomic run a week earlier (0.878, EI 3.923; per-row
scores and recall bytes differ, so it was re-generated, not cached). The HOLD
comes from the no-recall control moving +0.035 with no treatment applied. The
control arm's run-to-run spread is therefore larger than the gate's 0.02
tolerance, and the recall-minus-control delta across the three runs is
−0.006 / +0.012 / −0.023: this gate at n≈39 cannot resolve either the embedder
effect or the recall effect. The retrieval benchmarks above remain the only
measurement that separates the two embedders, and they favour Gemma.

**Decision (Seth, 2026-09-20): keep Gemma.** The retrieval benchmarks are the
embedder-separating measurement and the gate's movement is in the arm the embedder
cannot influence. The HOLD stays on record; the weekly gate re-runs in its normal
window and the doctor reports HOLD until a run PROMOTEs.
