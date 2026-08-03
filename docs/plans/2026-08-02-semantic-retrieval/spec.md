---
title: "Semantic retrieval — replace keyword graph lookup with on-device embeddings"
created: 2026-08-02
status: spec
audience: maintainers, research
---

# Semantic retrieval (#9) — spec

## Problem

Grounding matches **19.5%** of turns and returns a median of **1 node / 197 bytes**
when it does. That is not a tuning problem: retrieval is a keyword/graph lookup, so
"does he have a dog?" cannot reach a stored fact phrased "user owns French Bulldog".

The downstream cost is the tell human raters actually catch. At n=40 the dominant
detections were generic-where-Seth-is-specific: the model answered *"yeah that's the
corporate grind for you"* where the real Seth named Vanguard, LinkedIn and incoming
executives. A model cannot be specific about facts it cannot retrieve.

## What already exists (audited 2026-08-02, do not re-derive)

`src/memory/vector/` holds ~3,500 LOC, **none of it reachable from outside the
directory** — verified by grep for callers of `hu_vector_store_create` /
`hu_embeddings_generate`.

| File | LOC | State |
|---|---:|---|
| `chunker.c`, `cosine.c`, `math.c`, `circuit_breaker.c`, `outbox.c`, `provider_router.c` | ~560 | reusable |
| `embeddings_gemini.c` / `_voyage.c` / `_ollama.c` | 778 | remote; violate local-first |
| `embedder_local.c` | 168 | **fake** — hash-projected bag-of-words, no semantics |
| `store_qdrant.c` / `store_pgvector.c` | 647 | need an external server |
| `store_mem.c` | 303 | in-memory only, dies on restart |

So the gap is exactly two components plus wiring: **a real on-device embedder** and
**a local persistent store**.

## Decisions

### Store: vendored sqlite-vec, statically linked

`third_party/sqlite-vec/` (single C file exposing `sqlite3_vec_init`), compiled into
`human_core` and initialised at db open. Not a `.dylib`, not a runtime extension load
— h-uman's C SQLite never calls `sqlite3_enable_load_extension`, and adding that
would introduce a runtime file dependency the project's zero-dependency thesis
rejects. Vendoring follows the existing `third_party/llama.cpp` pattern.

Rejected: **uSearch** — better engine (HNSW/ANN, Apple-Silicon tuned) but its
advantage appears around 10^6 vectors. Our corpus is ~10^4 (2,964 opinions + 966
memories + a few thousand messages). Exact KNN at that size is sub-millisecond, so
ANN would add a C++ dependency to win a race we are not running. Revisit past ~10^6.

Rejected: **qdrant / pgvector** — already written, never wired, because both need a
server. That is why they sat unused; do not resurrect them.

### Embedder: nomic-embed-text-v2 through MLX

137M params, MIT, 8192-token context. ~300 MB beside the 56 GB GLM, so it does not
retrigger the two-model memory condition that has bitten this box repeatedly. MLX is
~50% faster than llama.cpp on embedding workloads (embedding is prefill-bound, where
MLX is best tuned) and MLX is already the serving stack.

Rejected: `bge-m3` (568M) — stronger and multilingual, worth revisiting given Seth
speaks Japanese, but 4x the size for a first cut. `Qwen3-Embedding-8B` — tops MTEB,
far too large to sit beside GLM.

Native means native: served in-process via MLX, **not** via Ollama HTTP. The existing
`embeddings_ollama.c` is the fast path, not the target, and is explicitly not the
Phase-1 deliverable.

## Phase 1 — prove semantic beats keyword, before wiring anything to production

**The deliverable is a measurement, not a feature.** Nothing touches the send path.

### The experiment

Paraphrase robustness is precisely what embeddings buy and keyword cannot do, so
that is what gets measured. For each of N stored facts, issue a query that means the
same thing in different words and record whether the fact is retrieved.

- Corpus: the real `memories` table (966 rows) plus extracted facts, embedded once.
- Queries: paraphrases that share little or no vocabulary with the stored text
  (`"user owns French Bulldog"` -> `"does he have a pet?"`).
- Metrics: **recall@1, recall@5, MRR**, keyword vs semantic, same corpus, same queries.
- Honest scoping: this measures paraphrase robustness specifically. It does NOT
  measure end-to-end reply quality, and must not be reported as if it did.

### Pass criterion (set BEFORE running)

Semantic recall@5 must exceed keyword recall@5 by **>= 20 absolute points** on the
paraphrase set. Below that, the embedder or the chunking is wrong and Phase 2 does
not start. A marginal win is not a win — it would not move a 19.5% match rate.

### Guards this must carry (each one has cost a session already)

- **Refuse, never fall back.** If the embedder is unavailable, exit non-zero and
  write no results file. A number produced by a different path is worse than none
  (`.claude/rules/no-number-without-a-measurement.md`).
- **Never score `embedder_local.c`.** Its hash-projection would produce a plausible,
  meaningless number. Assert the embedder identity in the results file.
- **Prove the guard discriminates** — run it against a deliberately broken embedder
  and confirm it fails for the right reason, not merely that it passes when healthy.
- **Report n.** recall on an empty or truncated query set is the `n=0` PASS trap.

## Phase 2 — wiring (only if Phase 1 passes)

1. `store_sqlite_vec.c` implementing the existing store vtable.
2. Backfill embeddings for `memories` + facts; incremental on write.
3. Hybrid rank: semantic + existing keyword, reusing `retrieval/hybrid.c`.
4. Ship behind `HU_SEMANTIC_RECALL=off|shadow|live` per
   `.claude/rules/feature-gate-requires-measurement.md`. SHADOW logs what it *would*
   have retrieved; LIVE requires a fresh blind-A/B on the corrected harness.

Note the harness was only just corrected (`bbe7e2a1e`) — before that it scored a
stale identity, so any pre-existing retrieval number is not a valid baseline.

## Non-goals

- Re-embedding the full 1,789-message history (start with `memories` + facts).
- Replacing the graph. GraphRAG stays; semantic recall is an additional retriever.
- Any change to the send path in Phase 1.
