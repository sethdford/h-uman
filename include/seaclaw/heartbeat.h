#ifndef SC_HEARTBEAT_H
#define SC_HEARTBEAT_H

#include "core/allocator.h"
#include "core/error.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Heartbeat engine — periodic health check / task execution
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_heartbeat_outcome {
    SC_HEARTBEAT_PROCESSED,
    SC_HEARTBEAT_SKIPPED_EMPTY,
    SC_HEARTBEAT_SKIPPED_MISSING,
} sc_heartbeat_outcome_t;

typedef struct sc_heartbeat_result {
    sc_heartbeat_outcome_t outcome;
    size_t task_count;
} sc_heartbeat_result_t;

typedef struct sc_heartbeat_engine {
    bool enabled;
    uint32_t interval_minutes;
    const char *workspace_dir;
} sc_heartbeat_engine_t;

/* Initialize. interval_minutes clamped to min 5. */
void sc_heartbeat_engine_init(sc_heartbeat_engine_t *engine,
                              bool enabled,
                              uint32_t interval_minutes,
                              const char *workspace_dir);

/* Parse tasks from content (lines starting with "- "). Caller frees each task
   and the array. Returns SC_OK, tasks/out_count set. */
sc_error_t sc_heartbeat_parse_tasks(sc_allocator_t *alloc,
                                    const char *content,
                                    char ***tasks_out,
                                    size_t *out_count);

/* Free tasks from parse_tasks. */
void sc_heartbeat_free_tasks(sc_allocator_t *alloc,
                             char **tasks,
                             size_t count);

/* Collect tasks from HEARTBEAT.md in workspace. Returns empty array if missing. */
sc_error_t sc_heartbeat_collect_tasks(sc_heartbeat_engine_t *engine,
                                      sc_allocator_t *alloc,
                                      char ***tasks_out,
                                      size_t *out_count);

/* Perform one tick: read HEARTBEAT.md, parse tasks. */
sc_error_t sc_heartbeat_tick(sc_heartbeat_engine_t *engine,
                             sc_allocator_t *alloc,
                             sc_heartbeat_result_t *result);

/* Create default HEARTBEAT.md if missing. */
sc_error_t sc_heartbeat_ensure_file(const char *workspace_dir,
                                    sc_allocator_t *alloc);

/* Check if content is effectively empty (comments/headers only). */
bool sc_heartbeat_is_empty_content(const char *content);

#endif /* SC_HEARTBEAT_H */
