#ifndef HU_MEMORY_RETRIEVAL_RRF_H
#define HU_MEMORY_RETRIEVAL_RRF_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>

/* Reciprocal Rank Fusion: merge ranked lists by key.
 * score = sum(1/(rank_i + k)) across sources.
 * Merges by content identity (key). */
hu_error_t hu_rrf_merge(hu_allocator_t *alloc, const hu_memory_entry_t **source_lists,
                        const size_t *source_lens, size_t num_sources, unsigned k, size_t limit,
                        hu_memory_entry_t **out, size_t *out_count);

/* Free result from hu_rrf_merge. */
void hu_rrf_free_result(hu_allocator_t *alloc, hu_memory_entry_t *entries, size_t count);

#endif /* HU_MEMORY_RETRIEVAL_RRF_H */
