#ifndef SC_MEMORY_LIFECYCLE_SEMANTIC_CACHE_H
#define SC_MEMORY_LIFECYCLE_SEMANTIC_CACHE_H

#include "seaclaw/memory/vector/embeddings.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

typedef struct sc_semantic_cache sc_semantic_cache_t;

typedef struct sc_semantic_cache_hit {
    char *response;
    float similarity;
    int semantic;  /* 1 if semantic match, 0 if exact hash */
} sc_semantic_cache_hit_t;

/* Create semantic cache. Uses embedding provider for similarity; falls back to exact match. */
sc_semantic_cache_t *sc_semantic_cache_create(sc_allocator_t *alloc,
    int ttl_minutes,
    size_t max_entries,
    float similarity_threshold,
    sc_embedding_provider_t *embedding_provider);  /* can be NULL for exact-only */

void sc_semantic_cache_destroy(sc_allocator_t *alloc, sc_semantic_cache_t *cache);

/* Lookup: key_hex is content hash, query_text optional for semantic match. */
sc_error_t sc_semantic_cache_get(sc_semantic_cache_t *cache,
    sc_allocator_t *alloc,
    const char *key_hex, size_t key_len,
    const char *query_text, size_t query_len,
    sc_semantic_cache_hit_t *out);  /* out->response allocated, caller frees */

/* Store response with optional query for embedding. */
sc_error_t sc_semantic_cache_put(sc_semantic_cache_t *cache,
    sc_allocator_t *alloc,
    const char *key_hex, size_t key_len,
    const char *model, size_t model_len,
    const char *response, size_t response_len,
    unsigned token_count,
    const char *query_text, size_t query_len);

void sc_semantic_cache_hit_free(sc_allocator_t *alloc, sc_semantic_cache_hit_t *hit);

#endif
