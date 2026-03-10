#ifndef HU_MEMORY_RETRIEVAL_QUERY_EXPANSION_H
#define HU_MEMORY_RETRIEVAL_QUERY_EXPANSION_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/* Expanded query for FTS5: filtered tokens, FTS query string. Caller frees. */
typedef struct hu_expanded_query {
    char *fts5_query;
    char **original_tokens;
    size_t original_count;
    char **filtered_tokens;
    size_t filtered_count;
} hu_expanded_query_t;

hu_error_t hu_query_expand(hu_allocator_t *alloc, const char *raw_query, size_t raw_len,
                           hu_expanded_query_t *out);
void hu_expanded_query_free(hu_allocator_t *alloc, hu_expanded_query_t *eq);

#endif /* HU_MEMORY_RETRIEVAL_QUERY_EXPANSION_H */
