#ifndef HU_MEMORY_LIFECYCLE_SEMANTIC_CACHE_H
#define HU_MEMORY_LIFECYCLE_SEMANTIC_CACHE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/vector/embeddings.h"
#include <stddef.h>

typedef struct hu_semantic_cache hu_semantic_cache_t;

typedef struct hu_semantic_cache_hit {
    char *response;
    float similarity;
    int semantic; /* 1 if semantic match, 0 if exact hash */
} hu_semantic_cache_hit_t;

/* Create semantic cache. Uses embedding provider for similarity; falls back to exact match. */
hu_semantic_cache_t *hu_semantic_cache_create(
    hu_allocator_t *alloc, int ttl_minutes, size_t max_entries, float similarity_threshold,
    hu_embedding_provider_t *embedding_provider); /* can be NULL for exact-only */

void hu_semantic_cache_destroy(hu_allocator_t *alloc, hu_semantic_cache_t *cache);

/* Lookup: key_hex is content hash, query_text optional for semantic match. */
hu_error_t
hu_semantic_cache_get(hu_semantic_cache_t *cache, hu_allocator_t *alloc, const char *key_hex,
                      size_t key_len, const char *query_text, size_t query_len,
                      hu_semantic_cache_hit_t *out); /* out->response allocated, caller frees */

/* Store response with optional query for embedding. */
hu_error_t hu_semantic_cache_put(hu_semantic_cache_t *cache, hu_allocator_t *alloc,
                                 const char *key_hex, size_t key_len, const char *model,
                                 size_t model_len, const char *response, size_t response_len,
                                 unsigned token_count, const char *query_text, size_t query_len);

void hu_semantic_cache_hit_free(hu_allocator_t *alloc, hu_semantic_cache_hit_t *hit);

#endif
