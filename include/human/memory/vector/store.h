#ifndef HU_MEMORY_VECTOR_STORE_H
#define HU_MEMORY_VECTOR_STORE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/* Vector search result: caller must free id and metadata */
typedef struct hu_vector_search_result {
    char *id;
    float score;
    char *metadata; /* optional JSON or text, can be NULL */
} hu_vector_search_result_t;

typedef struct hu_vector_store_vtable hu_vector_store_vtable_t;

typedef struct hu_vector_store {
    void *ctx;
    const hu_vector_store_vtable_t *vtable;
} hu_vector_store_t;

struct hu_vector_store_vtable {
    hu_error_t (*upsert)(void *ctx, hu_allocator_t *alloc, const char *id, size_t id_len,
                         const float *embedding, size_t dims, const char *metadata,
                         size_t metadata_len);
    hu_error_t (*search)(void *ctx, hu_allocator_t *alloc, const float *query_embedding,
                         size_t dims, size_t limit, hu_vector_search_result_t **results,
                         size_t *result_count);
    hu_error_t (*delete)(void *ctx, hu_allocator_t *alloc, const char *id, size_t id_len);
    size_t (*count)(void *ctx);
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
};

void hu_vector_search_results_free(hu_allocator_t *alloc, hu_vector_search_result_t *results,
                                   size_t count);

/* In-memory store implementing the vtable (for tests) */
hu_vector_store_t hu_vector_store_mem_vec_create(hu_allocator_t *alloc);

#endif
