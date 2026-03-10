#ifndef HU_TASK_LIST_H
#define HU_TASK_LIST_H
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_task_list_status {
    HU_TASK_LIST_PENDING,
    HU_TASK_LIST_CLAIMED,
    HU_TASK_LIST_IN_PROGRESS,
    HU_TASK_LIST_COMPLETED,
    HU_TASK_LIST_FAILED,
    HU_TASK_LIST_CANCELLED,
} hu_task_list_status_t;

typedef struct hu_task {
    uint64_t id;
    char *subject;
    char *description;
    uint64_t owner_agent_id; /* 0 = unclaimed */
    hu_task_list_status_t status;
    uint64_t *blocked_by; /* array of task IDs that must complete first */
    size_t blocked_by_count;
    int64_t created_at;
    int64_t updated_at;
} hu_task_t;

typedef struct hu_task_list hu_task_list_t;

/** Create task list. dir_path: optional; when set, tasks persist to dir_path/tasks.json.
 *  dir_path NULL = in-memory only. Under HU_IS_TEST, file I/O is skipped. */
hu_task_list_t *hu_task_list_create(hu_allocator_t *alloc, const char *dir_path, size_t max_tasks);
void hu_task_list_destroy(hu_task_list_t *list);

/* Add a new task, returns task ID */
hu_error_t hu_task_list_add(hu_task_list_t *list, const char *subject, const char *description,
                            const uint64_t *blocked_by, size_t blocked_by_count, uint64_t *out_id);

/* Claim an unclaimed, unblocked task for an agent */
hu_error_t hu_task_list_claim(hu_task_list_t *list, uint64_t task_id, uint64_t agent_id);

/* Update task status */
hu_error_t hu_task_list_update_status(hu_task_list_t *list, uint64_t task_id,
                                      hu_task_list_status_t status);

/* Get next available (unclaimed + unblocked) task */
hu_error_t hu_task_list_next_available(hu_task_list_t *list, hu_task_t *out);

/* Get task by ID */
hu_error_t hu_task_list_get(hu_task_list_t *list, uint64_t task_id, hu_task_t *out);

/* List all tasks */
hu_error_t hu_task_list_all(hu_task_list_t *list, hu_task_t **out, size_t *out_count);

/* Check if a task is blocked (any blocked_by task not yet COMPLETED) */
bool hu_task_list_is_blocked(hu_task_list_t *list, uint64_t task_id);

/* Count by status */
size_t hu_task_list_count_by_status(hu_task_list_t *list, hu_task_list_status_t status);

/* Check if a task's dependencies are all completed (ready to claim) */
bool hu_task_list_is_ready(hu_task_list_t *list, uint64_t task_id);

/* Get all tasks with a given status (out array must be freed with hu_task_array_free) */
hu_error_t hu_task_list_query(hu_task_list_t *list, hu_task_list_status_t status, hu_task_t **out,
                              size_t *out_count);

void hu_task_free(hu_allocator_t *alloc, hu_task_t *task);
void hu_task_array_free(hu_allocator_t *alloc, hu_task_t *tasks, size_t count);

#if defined(HU_IS_TEST) && HU_IS_TEST
/** Test-only: serialize tasks to JSON string. Caller frees with alloc->free. */
hu_error_t hu_task_list_serialize(hu_task_list_t *list, char **out_json, size_t *out_len);
/** Test-only: load tasks from JSON string (replaces current tasks). */
hu_error_t hu_task_list_deserialize(hu_task_list_t *list, const char *json, size_t json_len);
#endif
#endif
