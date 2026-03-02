#ifndef SC_MEMORY_VECTOR_STORE_H
#define SC_MEMORY_VECTOR_STORE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

/* Vector search result: caller must free id and metadata */
typedef struct sc_vector_search_result {
    char *id;
    float score;
    char *metadata;  /* optional JSON or text, can be NULL */
} sc_vector_search_result_t;

typedef struct sc_vector_store_vtable sc_vector_store_vtable_t;

typedef struct sc_vector_store {
    void *ctx;
    const sc_vector_store_vtable_t *vtable;
} sc_vector_store_t;

struct sc_vector_store_vtable {
    sc_error_t (*upsert)(void *ctx, sc_allocator_t *alloc,
        const char *id, size_t id_len,
        const float *embedding, size_t dims,
        const char *metadata, size_t metadata_len);
    sc_error_t (*search)(void *ctx, sc_allocator_t *alloc,
        const float *query_embedding, size_t dims,
        size_t limit,
        sc_vector_search_result_t **results, size_t *result_count);
    sc_error_t (*delete)(void *ctx, sc_allocator_t *alloc, const char *id, size_t id_len);
    size_t (*count)(void *ctx);
    void (*deinit)(void *ctx, sc_allocator_t *alloc);
};

void sc_vector_search_results_free(sc_allocator_t *alloc,
    sc_vector_search_result_t *results, size_t count);

/* In-memory store implementing the vtable (for tests) */
sc_vector_store_t sc_vector_store_mem_vec_create(sc_allocator_t *alloc);

#endif
