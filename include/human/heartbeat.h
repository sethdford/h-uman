#ifndef HU_HEARTBEAT_H
#define HU_HEARTBEAT_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Heartbeat engine — periodic health check / task execution
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_heartbeat_outcome {
    HU_HEARTBEAT_PROCESSED,
    HU_HEARTBEAT_SKIPPED_EMPTY,
    HU_HEARTBEAT_SKIPPED_MISSING,
} hu_heartbeat_outcome_t;

typedef struct hu_heartbeat_result {
    hu_heartbeat_outcome_t outcome;
    size_t task_count;
} hu_heartbeat_result_t;

typedef struct hu_heartbeat_engine {
    bool enabled;
    uint32_t interval_minutes;
    const char *workspace_dir;
} hu_heartbeat_engine_t;

/* Initialize. interval_minutes clamped to min 5. */
void hu_heartbeat_engine_init(hu_heartbeat_engine_t *engine, bool enabled,
                              uint32_t interval_minutes, const char *workspace_dir);

/* Parse tasks from content (lines starting with "- "). Caller frees each task
   and the array. Returns HU_OK, tasks/out_count set. */
hu_error_t hu_heartbeat_parse_tasks(hu_allocator_t *alloc, const char *content, char ***tasks_out,
                                    size_t *out_count);

/* Free tasks from parse_tasks. */
void hu_heartbeat_free_tasks(hu_allocator_t *alloc, char **tasks, size_t count);

/* Collect tasks from HEARTBEAT.md in workspace. Returns empty array if missing. */
hu_error_t hu_heartbeat_collect_tasks(hu_heartbeat_engine_t *engine, hu_allocator_t *alloc,
                                      char ***tasks_out, size_t *out_count);

/* Perform one tick: read HEARTBEAT.md, parse tasks. */
hu_error_t hu_heartbeat_tick(hu_heartbeat_engine_t *engine, hu_allocator_t *alloc,
                             hu_heartbeat_result_t *result);

/* Create default HEARTBEAT.md if missing. */
hu_error_t hu_heartbeat_ensure_file(const char *workspace_dir, hu_allocator_t *alloc);

/* Check if content is effectively empty (comments/headers only). */
bool hu_heartbeat_is_empty_content(const char *content);

#endif /* HU_HEARTBEAT_H */
