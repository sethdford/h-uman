#ifndef HUMAN_MEMORY_SEMANTIC_RECALL_H
#define HUMAN_MEMORY_SEMANTIC_RECALL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include "human/memory.h"
#include "human/memory/retrieval.h"
#include "human/memory/vector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Semantic recall (Phase 2 of docs/plans/2026-08-02-semantic-retrieval/spec.md).
 *
 * Gate: HU_SEMANTIC_RECALL=off|shadow|live (default OFF).
 *   OFF    — nothing changes; the hash embedder + empty in-memory store stay.
 *   SHADOW — real embedder + persistent store are attached and every recall
 *            computes the semantic candidates, LOGS them (count, overlap with
 *            keyword hits, fingerprint) and DROPS them.
 *   LIVE   — semantic candidates are merged into recall.
 * Promotion to LIVE is gated on the Phase-1 harness re-run through this path
 * (feature-gate-requires-measurement.md); SHADOW is what deploys first.
 *
 * Contract C1 (docs/plans/2026-08-02-semantic-retrieval/): SHADOW -> LIVE
 * additionally requires a blind A/B run via scripts/eval_semantic_live_gate.py
 * showing (a) the humanness composite is not lower under LIVE than SHADOW and
 * (b) neither the emotional-intelligence nor the reality-awareness judge axis
 * drops under LIVE. This is not optional test-passing: AlpsBench (arXiv
 * 2603.26680) found that adding memory retrieval improves persona awareness
 * but DEGRADES emotional intelligence and real-vs-hypothetical awareness via
 * over-reliance on retrieved memories — exactly the failure mode this gate
 * exists to catch before it reaches production replies. Do not flip this gate
 * to default-LIVE without a PROMOTE verdict from that script (see
 * docs/plans/2026-08-02-semantic-retrieval/semantic-live-gate-*.json for the
 * measurement record) — a green build and passing unit tests are not this
 * measurement (.claude/rules/feature-gate-requires-measurement.md,
 * .claude/rules/no-number-without-a-measurement.md). */
hu_gate_mode_t hu_semantic_recall_mode(void);

/* Embedding endpoint base URL: $HU_SEMANTIC_EMBED_URL, default the production
 * mlx-server (http://127.0.0.1:8741) which hosts /v1/embeddings in-process. */
const char *hu_semantic_recall_embed_url(void);

#define HU_SEMANTIC_EMBED_DIM 768u /* nomic-embed-text-v2 / modernbert-embed-base */

/* Build the real pair (HTTP embedder + sqlite-vec store on the engine's own
 * DB) and attach them to `mem` so writes are indexed. Returns
 * HU_ERR_NOT_SUPPORTED when `mem` is not a sqlite engine; HU_ERR_INTERNAL when
 * the store could not be created. On success the caller owns out_embedder /
 * out_store and must deinit them AFTER the memory engine. */
hu_error_t hu_semantic_recall_attach(hu_allocator_t *alloc, hu_memory_t *mem,
                                     hu_embedder_t *out_embedder, hu_vector_store_t *out_store);

/* Recall byte budget (2026-09-02 live-gate finding, docs/plans/
 * 2026-08-02-semantic-retrieval/semantic-live-gate-2026-09-02.json): 9 of 40
 * LIVE contexts returned an EMPTY completion where SHADOW returned none. The
 * LIVE arm differs from SHADOW only by the recall block — up to 5 hits of up
 * to 2000 chars each — which crowds the 16 KB prompt cap and the reply's
 * token budget. The block is therefore bounded at the SOURCE: each semantic
 * hit is cut at a word boundary to HU_SEMANTIC_RECALL_HIT_MAX_BYTES and the
 * whole semantic leg is capped at hu_semantic_recall_max_bytes(). The prompt
 * cap reserves the guard tail separately (hu_prompt_positional_cap_apply). */
#define HU_SEMANTIC_RECALL_DEFAULT_MAX_BYTES 1200u
#define HU_SEMANTIC_RECALL_HIT_MAX_BYTES     240u

/* $HU_SEMANTIC_RECALL_MAX_BYTES, default HU_SEMANTIC_RECALL_DEFAULT_MAX_BYTES.
 * Unparsable or non-positive values fail closed to the default. */
size_t hu_semantic_recall_max_bytes(void);

/* Pure: byte length to keep from s[0, len) so the result is <= max_bytes and
 * ends on a word boundary when one lies in the upper half of the window
 * (otherwise a hard cut at max_bytes, backed off any UTF-8 continuation
 * bytes). Returns len when len <= max_bytes; 0 on NULL / max_bytes == 0. */
size_t hu_semantic_recall_truncate_len(const char *s, size_t len, size_t max_bytes);

/* Clamp a semantic retrieval result in place: every entry's content is cut
 * to per_hit_bytes (word boundary), and entries are kept in rank order only
 * while the cumulative kept content stays within budget_bytes — the first
 * entry that would exceed it and every entry after it are dropped and freed.
 * entries/scores are shrunk so hu_retrieval_result_free stays exact.
 * Returns the total content bytes kept. Deterministic for identical input. */
size_t hu_semantic_recall_clamp_result(hu_allocator_t *alloc, hu_retrieval_result_t *res,
                                       size_t budget_bytes, size_t per_hit_bytes);

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_MEMORY_SEMANTIC_RECALL_H */
