#ifndef SC_MEMORY_VECTOR_STORE_QDRANT_H
#define SC_MEMORY_VECTOR_STORE_QDRANT_H

#include "seaclaw/memory/vector/store.h"
#include "seaclaw/core/allocator.h"

typedef struct sc_qdrant_config {
    const char *url;           /* e.g. "http://localhost:6333" */
    const char *api_key;       /* optional */
    const char *collection_name;
    size_t dimensions;
} sc_qdrant_config_t;

/* Create Qdrant store. Uses HTTP REST API. In SC_IS_TEST returns mock. */
sc_vector_store_t sc_vector_store_qdrant_create(sc_allocator_t *alloc,
    const sc_qdrant_config_t *config);

#endif
