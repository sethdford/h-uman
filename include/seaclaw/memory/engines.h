#ifndef SC_MEMORY_ENGINES_H
#define SC_MEMORY_ENGINES_H

#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

/* memory_lru — in-memory LRU cache */
sc_memory_t sc_memory_lru_create(sc_allocator_t *alloc, size_t max_entries);

/* postgres — PostgreSQL backend (stub when libpq not linked) */
sc_memory_t sc_postgres_memory_create(sc_allocator_t *alloc,
    const char *url, const char *schema, const char *table);

/* redis — Redis backend (stub when not linked) */
sc_memory_t sc_redis_memory_create(sc_allocator_t *alloc,
    const char *host, unsigned short port, const char *key_prefix);

/* lancedb — SQLite + vector backend (stub) */
sc_memory_t sc_lancedb_memory_create(sc_allocator_t *alloc, const char *db_path);

/* api — HTTP REST API backend (stub) */
sc_memory_t sc_api_memory_create(sc_allocator_t *alloc,
    const char *base_url, const char *api_key, unsigned int timeout_ms);

/* lucid — SQLite + lucid CLI backend (stub) */
sc_memory_t sc_lucid_memory_create(sc_allocator_t *alloc,
    const char *db_path, const char *workspace_dir);

#endif /* SC_MEMORY_ENGINES_H */
