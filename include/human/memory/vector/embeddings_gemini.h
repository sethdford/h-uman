#ifndef HU_MEMORY_VECTOR_EMBEDDINGS_GEMINI_H
#define HU_MEMORY_VECTOR_EMBEDDINGS_GEMINI_H

#include "human/core/allocator.h"
#include "human/memory/vector/embeddings.h"

/* Create Gemini embedding provider.
 * api_key: required for real API
 * model: optional, default "text-embedding-004"
 * dims: 0 = use default 768
 */
hu_embedding_provider_t hu_embedding_gemini_create(hu_allocator_t *alloc, const char *api_key,
                                                   const char *model, size_t dims);

#endif
