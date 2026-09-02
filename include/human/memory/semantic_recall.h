#ifndef HUMAN_MEMORY_SEMANTIC_RECALL_H
#define HUMAN_MEMORY_SEMANTIC_RECALL_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include "human/memory.h"
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
 * (feature-gate-requires-measurement.md); SHADOW is what deploys first. */
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

#ifdef __cplusplus
}
#endif

#endif /* HUMAN_MEMORY_SEMANTIC_RECALL_H */
