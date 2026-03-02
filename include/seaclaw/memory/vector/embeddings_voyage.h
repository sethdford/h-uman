#ifndef SC_MEMORY_VECTOR_EMBEDDINGS_VOYAGE_H
#define SC_MEMORY_VECTOR_EMBEDDINGS_VOYAGE_H

#include "seaclaw/memory/vector/embeddings.h"
#include "seaclaw/core/allocator.h"

/* Create Voyage embedding provider.
 * api_key: required for real API
 * model: optional, default "voyage-3-lite"
 * dims: 0 = use default 512
 */
sc_embedding_provider_t sc_embedding_voyage_create(sc_allocator_t *alloc,
    const char *api_key,
    const char *model,
    size_t dims);

#endif
