#ifndef HU_MEMORY_LIFECYCLE_MIGRATE_H
#define HU_MEMORY_LIFECYCLE_MIGRATE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/* Entry read from brain.db (OpenClaw/ZeroClaw SQLite) */
typedef struct hu_sqlite_source_entry {
    char *key;
    char *content;
    char *category;
} hu_sqlite_source_entry_t;

/* Read all entries from brain.db. Caller frees with hu_migrate_free_entries. */
hu_error_t hu_migrate_read_brain_db(hu_allocator_t *alloc, const char *db_path,
                                    hu_sqlite_source_entry_t **out, size_t *out_count);

/* Free entries from hu_migrate_read_brain_db. */
void hu_migrate_free_entries(hu_allocator_t *alloc, hu_sqlite_source_entry_t *entries,
                             size_t count);

#endif /* HU_MEMORY_LIFECYCLE_MIGRATE_H */
