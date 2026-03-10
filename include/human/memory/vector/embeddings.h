#ifndef HU_MEMORY_VECTOR_EMBEDDINGS_H
#define HU_MEMORY_VECTOR_EMBEDDINGS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/* Embedding result: caller owns values array, must call hu_embedding_provider_free */
typedef struct hu_embedding_provider_result {
    float *values;
    size_t dimensions;
} hu_embedding_provider_result_t;

/* Vtable for embedding providers (Gemini, Ollama, Voyage, etc.) */
typedef struct hu_embedding_provider_vtable hu_embedding_provider_vtable_t;

typedef struct hu_embedding_provider {
    void *ctx;
    const hu_embedding_provider_vtable_t *vtable;
} hu_embedding_provider_t;

struct hu_embedding_provider_vtable {
    hu_error_t (*embed)(void *ctx, hu_allocator_t *alloc, const char *text, size_t text_len,
                        hu_embedding_provider_result_t *out);
    const char *(*name)(void *ctx);
    size_t (*dimensions)(void *ctx);
    void (*deinit)(void *ctx, hu_allocator_t *alloc);
};

/* Free embedding result (values array). Safe to call with NULL out. */
void hu_embedding_provider_free(hu_allocator_t *alloc, hu_embedding_provider_result_t *out);

/* Noop provider: returns empty vector, keyword-only fallback */
hu_embedding_provider_t hu_embedding_provider_noop_create(hu_allocator_t *alloc);

/* Factory: create provider by name. Returns noop for unknown. */
hu_embedding_provider_t hu_embedding_provider_create(hu_allocator_t *alloc,
                                                     const char *provider_name, const char *api_key,
                                                     const char *model, size_t dims);

#endif
