#ifndef HU_MEMORY_VECTOR_STORE_PGVECTOR_H
#define HU_MEMORY_VECTOR_STORE_PGVECTOR_H

#include "human/core/allocator.h"
#include "human/memory/vector/store.h"

typedef struct hu_pgvector_config {
    const char *connection_url;
    const char *table_name; /* default "memory_vectors" */
    size_t dimensions;
} hu_pgvector_config_t;

/* Create pgvector store. When HU_ENABLE_POSTGRES is not defined, returns store that fails all ops.
 */
hu_vector_store_t hu_vector_store_pgvector_create(hu_allocator_t *alloc,
                                                  const hu_pgvector_config_t *config);

#endif
