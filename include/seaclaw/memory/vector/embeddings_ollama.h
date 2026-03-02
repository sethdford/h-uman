#ifndef SC_MEMORY_VECTOR_EMBEDDINGS_OLLAMA_H
#define SC_MEMORY_VECTOR_EMBEDDINGS_OLLAMA_H

#include "seaclaw/memory/vector/embeddings.h"
#include "seaclaw/core/allocator.h"

/* Create Ollama embedding provider.
 * model: optional, default "nomic-embed-text"
 * dims: 0 = use default 768
 */
sc_embedding_provider_t sc_embedding_ollama_create(sc_allocator_t *alloc,
    const char *model,
    size_t dims);

#endif
