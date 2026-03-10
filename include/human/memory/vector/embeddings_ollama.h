#ifndef HU_MEMORY_VECTOR_EMBEDDINGS_OLLAMA_H
#define HU_MEMORY_VECTOR_EMBEDDINGS_OLLAMA_H

#include "human/core/allocator.h"
#include "human/memory/vector/embeddings.h"

/* Create Ollama embedding provider.
 * model: optional, default "nomic-embed-text"
 * dims: 0 = use default 768
 */
hu_embedding_provider_t hu_embedding_ollama_create(hu_allocator_t *alloc, const char *model,
                                                   size_t dims);

#endif
