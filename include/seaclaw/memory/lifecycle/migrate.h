#ifndef SC_MEMORY_LIFECYCLE_MIGRATE_H
#define SC_MEMORY_LIFECYCLE_MIGRATE_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stddef.h>

/* Entry read from brain.db (OpenClaw/ZeroClaw SQLite) */
typedef struct sc_sqlite_source_entry {
    char *key;
    char *content;
    char *category;
} sc_sqlite_source_entry_t;

/* Read all entries from brain.db. Caller frees with sc_migrate_free_entries. */
sc_error_t sc_migrate_read_brain_db(sc_allocator_t *alloc, const char *db_path,
    sc_sqlite_source_entry_t **out, size_t *out_count);

/* Free entries from sc_migrate_read_brain_db. */
void sc_migrate_free_entries(sc_allocator_t *alloc,
    sc_sqlite_source_entry_t *entries, size_t count);

#endif /* SC_MEMORY_LIFECYCLE_MIGRATE_H */
