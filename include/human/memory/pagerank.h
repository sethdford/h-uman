/*
 * W12 — HippoRAG-style personalized PageRank over the per-contact entity
 * graph. Pure CPU power-iteration. The function is the "soft retrieval"
 * primitive used by the planner when a goal anchors on multiple seed
 * entities and we want to score every reachable entity by graph-walk
 * affinity (prior art: HippoRAG arxiv 2405.14831).
 *
 * Caps:
 *   - Entity count <= HU_PAGERANK_MAX_ENTITIES (10000). Larger graphs
 *     return HU_ERR_INVALID_ARGUMENT — W14 sleep-compute will materialise
 *     hot subgraphs to stay under this ceiling.
 *   - iterations defaults to HU_PAGERANK_DEFAULT_ITERATIONS (20) when 0.
 *   - damping defaults to HU_PAGERANK_DEFAULT_DAMPING (0.85) when <= 0
 *     or >= 1. Out-of-range values are clamped silently (KISS).
 *
 * Output arrays are sorted by score descending; both must be freed by the
 * caller via the supplied allocator's free() method.
 */
#ifndef HU_MEMORY_PAGERANK_H
#define HU_MEMORY_PAGERANK_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/memory.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_PAGERANK_MAX_ENTITIES        10000
#define HU_PAGERANK_DEFAULT_ITERATIONS  20
#define HU_PAGERANK_DEFAULT_DAMPING     0.85f

/* Personalized PageRank from `seed_entity_ids`. If `seeds_count == 0` the
 * function returns `*out_count = 0` and HU_OK (caller's choice — empty seed
 * means "no preference"; HippoRAG treats this as a no-op rather than
 * uniform PageRank, which is rarely what callers want). */
hu_error_t hu_memory_pagerank_seeds(hu_memory_t *m, hu_allocator_t *alloc,
                                    const char *contact_id, size_t cid_len,
                                    const int64_t *seed_entity_ids, size_t seeds_count,
                                    float damping, size_t iterations,
                                    int64_t **out_ids, float **out_scores,
                                    size_t *out_count);

#ifdef __cplusplus
}
#endif

#endif /* HU_MEMORY_PAGERANK_H */
