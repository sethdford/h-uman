#ifndef SC_MEMORY_RETRIEVAL_QUERY_EXPANSION_H
#define SC_MEMORY_RETRIEVAL_QUERY_EXPANSION_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

/* Expanded query for FTS5: filtered tokens, FTS query string. Caller frees. */
typedef struct sc_expanded_query {
    char *fts5_query;
    char **original_tokens;
    size_t original_count;
    char **filtered_tokens;
    size_t filtered_count;
} sc_expanded_query_t;

sc_error_t sc_query_expand(sc_allocator_t *alloc, const char *raw_query, size_t raw_len,
    sc_expanded_query_t *out);
void sc_expanded_query_free(sc_allocator_t *alloc, sc_expanded_query_t *eq);

#endif /* SC_MEMORY_RETRIEVAL_QUERY_EXPANSION_H */
