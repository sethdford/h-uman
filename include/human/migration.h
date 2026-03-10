#ifndef HU_MIGRATION_H
#define HU_MIGRATION_H

#include "core/allocator.h"
#include "core/error.h"
#include "memory.h"
#include <stddef.h>

typedef enum hu_migration_source {
    HU_MIGRATION_SOURCE_NONE,
    HU_MIGRATION_SOURCE_SQLITE,
    HU_MIGRATION_SOURCE_MARKDOWN,
} hu_migration_source_t;

typedef enum hu_migration_target {
    HU_MIGRATION_TARGET_SQLITE,
    HU_MIGRATION_TARGET_MARKDOWN,
} hu_migration_target_t;

typedef struct hu_migration_config {
    hu_migration_source_t source;
    hu_migration_target_t target;
    const char *source_path; /* db path for sqlite, dir for markdown */
    size_t source_path_len;
    const char *target_path;
    size_t target_path_len;
    bool dry_run;
} hu_migration_config_t;

typedef struct hu_migration_stats {
    size_t from_sqlite;
    size_t from_markdown;
    size_t imported;
    size_t skipped;
    size_t errors;
} hu_migration_stats_t;

typedef void (*hu_migration_progress_fn)(void *ctx, size_t current, size_t total);

/**
 * Migrate data between memory backends.
 * Supports: none→sqlite, sqlite→markdown, markdown→sqlite.
 * Uses memory vtable: read from source via list/get, write to target via store.
 */
hu_error_t hu_migration_run(hu_allocator_t *alloc, const hu_migration_config_t *cfg,
                            hu_migration_stats_t *out_stats, hu_migration_progress_fn progress,
                            void *progress_ctx);

#endif /* HU_MIGRATION_H */
