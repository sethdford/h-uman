#ifndef SC_MEMORY_RETRIEVAL_RRF_H
#define SC_MEMORY_RETRIEVAL_RRF_H

#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

/* Reciprocal Rank Fusion: merge ranked lists by key.
 * score = sum(1/(rank_i + k)) across sources.
 * Merges by content identity (key). */
sc_error_t sc_rrf_merge(sc_allocator_t *alloc,
    const sc_memory_entry_t **source_lists, const size_t *source_lens, size_t num_sources,
    unsigned k, size_t limit,
    sc_memory_entry_t **out, size_t *out_count);

/* Free result from sc_rrf_merge. */
void sc_rrf_free_result(sc_allocator_t *alloc,
    sc_memory_entry_t *entries, size_t count);

#endif /* SC_MEMORY_RETRIEVAL_RRF_H */
