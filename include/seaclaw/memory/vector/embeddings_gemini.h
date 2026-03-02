#ifndef SC_MEMORY_VECTOR_EMBEDDINGS_GEMINI_H
#define SC_MEMORY_VECTOR_EMBEDDINGS_GEMINI_H

#include "seaclaw/memory/vector/embeddings.h"
#include "seaclaw/core/allocator.h"

/* Create Gemini embedding provider.
 * api_key: required for real API
 * model: optional, default "text-embedding-004"
 * dims: 0 = use default 768
 */
sc_embedding_provider_t sc_embedding_gemini_create(sc_allocator_t *alloc,
    const char *api_key,
    const char *model,
    size_t dims);

#endif
