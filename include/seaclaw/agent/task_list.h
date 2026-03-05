#ifndef SC_TASK_LIST_H
#define SC_TASK_LIST_H
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum sc_task_list_status {
    SC_TASK_LIST_PENDING,
    SC_TASK_LIST_CLAIMED,
    SC_TASK_LIST_IN_PROGRESS,
    SC_TASK_LIST_COMPLETED,
    SC_TASK_LIST_FAILED,
    SC_TASK_LIST_CANCELLED,
} sc_task_list_status_t;

typedef struct sc_task {
    uint64_t id;
    char *subject;
    char *description;
    uint64_t owner_agent_id; /* 0 = unclaimed */
    sc_task_list_status_t status;
    uint64_t *blocked_by; /* array of task IDs that must complete first */
    size_t blocked_by_count;
    int64_t created_at;
    int64_t updated_at;
} sc_task_t;

typedef struct sc_task_list sc_task_list_t;

sc_task_list_t *sc_task_list_create(sc_allocator_t *alloc, size_t max_tasks);
void sc_task_list_destroy(sc_task_list_t *list);

/* Add a new task, returns task ID */
sc_error_t sc_task_list_add(sc_task_list_t *list, const char *subject, const char *description,
                            const uint64_t *blocked_by, size_t blocked_by_count, uint64_t *out_id);

/* Claim an unclaimed, unblocked task for an agent */
sc_error_t sc_task_list_claim(sc_task_list_t *list, uint64_t task_id, uint64_t agent_id);

/* Update task status */
sc_error_t sc_task_list_update_status(sc_task_list_t *list, uint64_t task_id,
                                      sc_task_list_status_t status);

/* Get next available (unclaimed + unblocked) task */
sc_error_t sc_task_list_next_available(sc_task_list_t *list, sc_task_t *out);

/* Get task by ID */
sc_error_t sc_task_list_get(sc_task_list_t *list, uint64_t task_id, sc_task_t *out);

/* List all tasks */
sc_error_t sc_task_list_all(sc_task_list_t *list, sc_task_t **out, size_t *out_count);

/* Check if a task is blocked (any blocked_by task not yet COMPLETED) */
bool sc_task_list_is_blocked(sc_task_list_t *list, uint64_t task_id);

/* Count by status */
size_t sc_task_list_count_by_status(sc_task_list_t *list, sc_task_list_status_t status);

/* Check if a task's dependencies are all completed (ready to claim) */
bool sc_task_list_is_ready(sc_task_list_t *list, uint64_t task_id);

/* Get all tasks with a given status (out array must be freed with sc_task_array_free) */
sc_error_t sc_task_list_query(sc_task_list_t *list, sc_task_list_status_t status,
                              sc_task_t **out, size_t *out_count);

void sc_task_free(sc_allocator_t *alloc, sc_task_t *task);
void sc_task_array_free(sc_allocator_t *alloc, sc_task_t *tasks, size_t count);
#endif
