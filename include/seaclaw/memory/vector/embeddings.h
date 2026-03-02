#ifndef SC_MEMORY_VECTOR_EMBEDDINGS_H
#define SC_MEMORY_VECTOR_EMBEDDINGS_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

/* Embedding result: caller owns values array, must call sc_embedding_provider_free */
typedef struct sc_embedding_provider_result {
    float *values;
    size_t dimensions;
} sc_embedding_provider_result_t;

/* Vtable for embedding providers (Gemini, Ollama, Voyage, etc.) */
typedef struct sc_embedding_provider_vtable sc_embedding_provider_vtable_t;

typedef struct sc_embedding_provider {
    void *ctx;
    const sc_embedding_provider_vtable_t *vtable;
} sc_embedding_provider_t;

struct sc_embedding_provider_vtable {
    sc_error_t (*embed)(void *ctx, sc_allocator_t *alloc,
        const char *text, size_t text_len,
        sc_embedding_provider_result_t *out);
    const char *(*name)(void *ctx);
    size_t (*dimensions)(void *ctx);
    void (*deinit)(void *ctx, sc_allocator_t *alloc);
};

/* Free embedding result (values array). Safe to call with NULL out. */
void sc_embedding_provider_free(sc_allocator_t *alloc,
    sc_embedding_provider_result_t *out);

/* Noop provider: returns empty vector, keyword-only fallback */
sc_embedding_provider_t sc_embedding_provider_noop_create(sc_allocator_t *alloc);

/* Factory: create provider by name. Returns noop for unknown. */
sc_embedding_provider_t sc_embedding_provider_create(sc_allocator_t *alloc,
    const char *provider_name,
    const char *api_key,
    const char *model,
    size_t dims);

#endif
