#ifndef SC_SUBAGENT_H
#define SC_SUBAGENT_H

#include "seaclaw/config.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum sc_task_status {
    SC_TASK_RUNNING,
    SC_TASK_COMPLETED,
    SC_TASK_FAILED,
} sc_task_status_t;

typedef struct sc_subagent_manager sc_subagent_manager_t;

sc_subagent_manager_t *sc_subagent_create(sc_allocator_t *alloc, const sc_config_t *cfg);
void sc_subagent_destroy(sc_allocator_t *alloc, sc_subagent_manager_t *mgr);

sc_error_t sc_subagent_spawn(sc_subagent_manager_t *mgr, const char *task, size_t task_len,
                             const char *label, const char *channel, const char *chat_id,
                             uint64_t *out_task_id);

sc_task_status_t sc_subagent_get_status(sc_subagent_manager_t *mgr, uint64_t task_id);
const char *sc_subagent_get_result(sc_subagent_manager_t *mgr, uint64_t task_id);
size_t sc_subagent_running_count(sc_subagent_manager_t *mgr);

sc_error_t sc_subagent_cancel(sc_subagent_manager_t *mgr, uint64_t task_id);

typedef struct sc_subagent_task_info {
    uint64_t task_id;
    sc_task_status_t status;
    const char *label;
    const char *task;
    int64_t started_at;
} sc_subagent_task_info_t;

sc_error_t sc_subagent_get_all(sc_subagent_manager_t *mgr, sc_allocator_t *alloc,
                               sc_subagent_task_info_t **out, size_t *out_count);

#endif /* SC_SUBAGENT_H */
