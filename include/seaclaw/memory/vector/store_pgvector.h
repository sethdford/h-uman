#ifndef SC_MEMORY_VECTOR_STORE_PGVECTOR_H
#define SC_MEMORY_VECTOR_STORE_PGVECTOR_H

#include "seaclaw/memory/vector/store.h"
#include "seaclaw/core/allocator.h"

typedef struct sc_pgvector_config {
    const char *connection_url;
    const char *table_name;  /* default "memory_vectors" */
    size_t dimensions;
} sc_pgvector_config_t;

/* Create pgvector store. When SC_ENABLE_POSTGRES is not defined, returns store that fails all ops. */
sc_vector_store_t sc_vector_store_pgvector_create(sc_allocator_t *alloc,
    const sc_pgvector_config_t *config);

#endif
