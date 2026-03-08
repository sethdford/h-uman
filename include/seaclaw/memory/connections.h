#ifndef SC_MEMORY_CONNECTIONS_H
#define SC_MEMORY_CONNECTIONS_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/memory.h"
#include <stddef.h>

#define SC_CONN_MAX_CONNECTIONS 20
#define SC_CONN_MAX_INSIGHTS    10

typedef struct sc_memory_connection {
    size_t memory_a_idx;
    size_t memory_b_idx;
    char *relationship;
    size_t relationship_len;
    double strength;
} sc_memory_connection_t;

typedef struct sc_memory_insight {
    char *text;
    size_t text_len;
    size_t *related_indices;
    size_t related_count;
} sc_memory_insight_t;

typedef struct sc_connection_result {
    sc_memory_connection_t connections[SC_CONN_MAX_CONNECTIONS];
    size_t connection_count;
    sc_memory_insight_t insights[SC_CONN_MAX_INSIGHTS];
    size_t insight_count;
} sc_connection_result_t;

sc_error_t sc_connections_build_prompt(sc_allocator_t *alloc, const sc_memory_entry_t *entries,
                                       size_t entry_count, char **out, size_t *out_len);

sc_error_t sc_connections_parse(sc_allocator_t *alloc, const char *response, size_t response_len,
                                size_t entry_count, sc_connection_result_t *out);

sc_error_t sc_connections_store_insights(sc_allocator_t *alloc, sc_memory_t *memory,
                                         const sc_connection_result_t *result,
                                         const sc_memory_entry_t *entries, size_t entry_count);

void sc_connection_result_deinit(sc_connection_result_t *result, sc_allocator_t *alloc);

#endif
