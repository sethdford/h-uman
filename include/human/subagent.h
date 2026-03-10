#ifndef HU_SUBAGENT_H
#define HU_SUBAGENT_H

#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_task_status {
    HU_TASK_RUNNING,
    HU_TASK_COMPLETED,
    HU_TASK_FAILED,
} hu_task_status_t;

typedef struct hu_subagent_manager hu_subagent_manager_t;

hu_subagent_manager_t *hu_subagent_create(hu_allocator_t *alloc, const hu_config_t *cfg);
void hu_subagent_destroy(hu_allocator_t *alloc, hu_subagent_manager_t *mgr);

hu_error_t hu_subagent_spawn(hu_subagent_manager_t *mgr, const char *task, size_t task_len,
                             const char *label, const char *channel, const char *chat_id,
                             uint64_t *out_task_id);

hu_task_status_t hu_subagent_get_status(hu_subagent_manager_t *mgr, uint64_t task_id);
const char *hu_subagent_get_result(hu_subagent_manager_t *mgr, uint64_t task_id);
size_t hu_subagent_running_count(hu_subagent_manager_t *mgr);

hu_error_t hu_subagent_cancel(hu_subagent_manager_t *mgr, uint64_t task_id);

typedef struct hu_subagent_task_info {
    uint64_t task_id;
    hu_task_status_t status;
    const char *label;
    const char *task;
    int64_t started_at;
} hu_subagent_task_info_t;

hu_error_t hu_subagent_get_all(hu_subagent_manager_t *mgr, hu_allocator_t *alloc,
                               hu_subagent_task_info_t **out, size_t *out_count);

#endif /* HU_SUBAGENT_H */
