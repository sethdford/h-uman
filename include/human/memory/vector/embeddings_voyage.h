#ifndef HU_MEMORY_VECTOR_EMBEDDINGS_VOYAGE_H
#define HU_MEMORY_VECTOR_EMBEDDINGS_VOYAGE_H

#include "human/core/allocator.h"
#include "human/memory/vector/embeddings.h"

/* Create Voyage embedding provider.
 * api_key: required for real API
 * model: optional, default "voyage-3-lite"
 * dims: 0 = use default 512
 */
hu_embedding_provider_t hu_embedding_voyage_create(hu_allocator_t *alloc, const char *api_key,
                                                   const char *model, size_t dims);

#endif
