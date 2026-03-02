#ifndef SC_MIGRATION_H
#define SC_MIGRATION_H

#include "core/allocator.h"
#include "core/error.h"
#include "memory.h"
#include <stddef.h>

typedef enum sc_migration_source {
    SC_MIGRATION_SOURCE_NONE,
    SC_MIGRATION_SOURCE_SQLITE,
    SC_MIGRATION_SOURCE_MARKDOWN,
} sc_migration_source_t;

typedef enum sc_migration_target {
    SC_MIGRATION_TARGET_SQLITE,
    SC_MIGRATION_TARGET_MARKDOWN,
} sc_migration_target_t;

typedef struct sc_migration_config {
    sc_migration_source_t source;
    sc_migration_target_t target;
    const char *source_path;   /* db path for sqlite, dir for markdown */
    size_t source_path_len;
    const char *target_path;
    size_t target_path_len;
    bool dry_run;
} sc_migration_config_t;

typedef struct sc_migration_stats {
    size_t from_sqlite;
    size_t from_markdown;
    size_t imported;
    size_t skipped;
    size_t errors;
} sc_migration_stats_t;

typedef void (*sc_migration_progress_fn)(void *ctx, size_t current, size_t total);

/**
 * Migrate data between memory backends.
 * Supports: none→sqlite, sqlite→markdown, markdown→sqlite.
 * Uses memory vtable: read from source via list/get, write to target via store.
 */
sc_error_t sc_migration_run(sc_allocator_t *alloc,
    const sc_migration_config_t *cfg,
    sc_migration_stats_t *out_stats,
    sc_migration_progress_fn progress,
    void *progress_ctx);

#endif /* SC_MIGRATION_H */
