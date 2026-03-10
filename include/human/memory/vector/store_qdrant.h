#ifndef HU_MEMORY_VECTOR_STORE_QDRANT_H
#define HU_MEMORY_VECTOR_STORE_QDRANT_H

#include "human/core/allocator.h"
#include "human/memory/vector/store.h"

typedef struct hu_qdrant_config {
    const char *url;     /* e.g. "http://localhost:6333" */
    const char *api_key; /* optional */
    const char *collection_name;
    size_t dimensions;
} hu_qdrant_config_t;

/* Create Qdrant store. Uses HTTP REST API. In HU_IS_TEST returns mock. */
hu_vector_store_t hu_vector_store_qdrant_create(hu_allocator_t *alloc,
                                                const hu_qdrant_config_t *config);

#endif
