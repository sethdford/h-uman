#include "human/task_manager.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

#define HU_TASK_MANAGER_DEFAULT_MAX 1000

struct hu_task_manager {
    hu_task_t *tasks;
    size_t count;
    size_t capacity;
    uint32_t next_id;
};

hu_error_t hu_task_manager_create(hu_allocator_t *alloc, size_t max_tasks,
                                  hu_task_manager_t **out) {
    if (!alloc || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t capacity = max_tasks > 0 ? max_tasks : HU_TASK_MANAGER_DEFAULT_MAX;

    hu_task_manager_t *mgr = (hu_task_manager_t *)alloc->alloc(alloc->ctx, sizeof(hu_task_manager_t));
    if (!mgr) {
        return HU_ERR_OUT_OF_MEMORY;
    }

    mgr->tasks = (hu_task_t *)alloc->alloc(alloc->ctx, capacity * sizeof(hu_task_t));
    if (!mgr->tasks) {
        alloc->free(alloc->ctx, mgr, sizeof(hu_task_manager_t));
        return HU_ERR_OUT_OF_MEMORY;
    }

    memset(mgr->tasks, 0, capacity * sizeof(hu_task_t));
    mgr->count = 0;
    mgr->capacity = capacity;
    mgr->next_id = 1;

    *out = mgr;
    return HU_OK;
}

hu_error_t hu_task_manager_add(hu_task_manager_t *mgr, hu_allocator_t *alloc,
                               const char *subject, size_t subject_len, const char *description,
                               size_t description_len, uint32_t *out_id) {
    if (!mgr || !alloc || !subject || !description || !out_id) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    if (mgr->count >= mgr->capacity) {
        return HU_ERR_OUT_OF_MEMORY;
    }

    hu_task_t *task = &mgr->tasks[mgr->count];
    task->id = mgr->next_id;

    /* Copy subject */
    task->subject = hu_strndup(alloc, subject, subject_len);
    if (!task->subject) {
        return HU_ERR_OUT_OF_MEMORY;
    }
    task->subject_len = subject_len;

    /* Copy description */
    task->description = hu_strndup(alloc, description, description_len);
    if (!task->description) {
        alloc->free(alloc->ctx, task->subject, subject_len + 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    task->description_len = description_len;

    task->status = HU_TASK_PENDING;

    *out_id = mgr->next_id;
    mgr->next_id++;
    mgr->count++;

    return HU_OK;
}

hu_error_t hu_task_manager_update_status(hu_task_manager_t *mgr, uint32_t id,
                                         hu_task_status_t status) {
    if (!mgr) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->tasks[i].id == id) {
            mgr->tasks[i].status = status;
            return HU_OK;
        }
    }

    return HU_ERR_NOT_FOUND;
}

hu_error_t hu_task_manager_get(const hu_task_manager_t *mgr, uint32_t id,
                               const hu_task_t **out) {
    if (!mgr || !out) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->tasks[i].id == id) {
            *out = &mgr->tasks[i];
            return HU_OK;
        }
    }

    return HU_ERR_NOT_FOUND;
}

static const char *status_to_string(hu_task_status_t status) {
    switch (status) {
    case HU_TASK_PENDING:
        return "pending";
    case HU_TASK_IN_PROGRESS:
        return "in_progress";
    case HU_TASK_COMPLETED:
        return "completed";
    default:
        return "unknown";
    }
}

hu_error_t hu_task_manager_list(const hu_task_manager_t *mgr, hu_allocator_t *alloc,
                                char **out_json, size_t *out_len) {
    if (!mgr || !alloc || !out_json || !out_len) {
        return HU_ERR_INVALID_ARGUMENT;
    }

    /* Estimate buffer size: 256 bytes per task + 2 for brackets */
    size_t buf_size = 2 + (mgr->count * 256);
    char *buf = (char *)alloc->alloc(alloc->ctx, buf_size);
    if (!buf) {
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t pos = 0;
    buf[pos++] = '[';

    for (size_t i = 0; i < mgr->count; i++) {
        const hu_task_t *task = &mgr->tasks[i];
        const char *status_str = status_to_string(task->status);

        if (i > 0 && pos < buf_size - 1) {
            buf[pos++] = ',';
        }

        int wrote = snprintf(buf + pos, buf_size - pos,
                             "{\"id\":%u,\"subject\":\"%.*s\",\"description\":\"%.*s\","
                             "\"status\":\"%s\"}",
                             task->id, (int)task->subject_len, task->subject,
                             (int)(task->description_len > 100 ? 100 : task->description_len),
                             task->description, status_str);

        if (wrote > 0 && (size_t)wrote < buf_size - pos) {
            pos += (size_t)wrote;
        }
    }

    if (pos < buf_size - 1) {
        buf[pos++] = ']';
    }
    buf[pos] = '\0';

    *out_json = buf;
    *out_len = pos;
    return HU_OK;
}

void hu_task_manager_destroy(hu_task_manager_t *mgr, hu_allocator_t *alloc) {
    if (!mgr || !alloc) {
        return;
    }

    for (size_t i = 0; i < mgr->count; i++) {
        if (mgr->tasks[i].subject) {
            alloc->free(alloc->ctx, mgr->tasks[i].subject, mgr->tasks[i].subject_len + 1);
        }
        if (mgr->tasks[i].description) {
            alloc->free(alloc->ctx, mgr->tasks[i].description, mgr->tasks[i].description_len + 1);
        }
    }

    if (mgr->tasks) {
        alloc->free(alloc->ctx, mgr->tasks, mgr->capacity * sizeof(hu_task_t));
    }

    alloc->free(alloc->ctx, mgr, sizeof(hu_task_manager_t));
}
