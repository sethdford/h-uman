#ifndef HU_MEMORY_VECTOR_EMBEDDER_GEMINI_ADAPTER_H
#define HU_MEMORY_VECTOR_EMBEDDER_GEMINI_ADAPTER_H

#include "human/core/allocator.h"
#include "human/memory/vector.h"
#include "human/memory/vector/embeddings.h"

/* Create an hu_embedder_t that wraps a Gemini embedding provider.
 * The adapter forwards embed/embed_batch/dimensions/deinit to the provider.
 * embed_batch is implemented by looping over embed (Gemini has no native batch). */
hu_embedder_t hu_embedder_gemini_adapter_create(hu_allocator_t *alloc,
                                                 hu_embedding_provider_t provider);

#endif /* HU_MEMORY_VECTOR_EMBEDDER_GEMINI_ADAPTER_H */
