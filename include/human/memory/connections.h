#ifndef HU_MEMORY_CONNECTIONS_H
#define HU_MEMORY_CONNECTIONS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>

#define HU_CONN_MAX_CONNECTIONS 20
#define HU_CONN_MAX_INSIGHTS    10
#define HU_CONN_PROMPT_CAP      4096

typedef struct hu_memory_connection {
    size_t memory_a_idx;
    size_t memory_b_idx;
    char *relationship;
    size_t relationship_len;
    double strength;
} hu_memory_connection_t;

typedef struct hu_memory_insight {
    char *text;
    size_t text_len;
    size_t *related_indices;
    size_t related_count;
    size_t related_alloc_bytes;
} hu_memory_insight_t;

typedef struct hu_connection_result {
    hu_memory_connection_t connections[HU_CONN_MAX_CONNECTIONS];
    size_t connection_count;
    hu_memory_insight_t insights[HU_CONN_MAX_INSIGHTS];
    size_t insight_count;
} hu_connection_result_t;

hu_error_t hu_connections_build_prompt(hu_allocator_t *alloc, const hu_memory_entry_t *entries,
                                       size_t entry_count, char **out, size_t *out_len);

hu_error_t hu_connections_parse(hu_allocator_t *alloc, const char *response, size_t response_len,
                                size_t entry_count, hu_connection_result_t *out);

hu_error_t hu_connections_store_insights(hu_allocator_t *alloc, hu_memory_t *memory,
                                         const hu_connection_result_t *result,
                                         const hu_memory_entry_t *entries, size_t entry_count);

void hu_connection_result_deinit(hu_connection_result_t *result, hu_allocator_t *alloc);

#endif
