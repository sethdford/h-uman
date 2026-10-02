#include "human/agent/checkpoint.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

void hu_checkpoint_store_init(hu_checkpoint_store_t *store, bool auto_checkpoint,
                              uint32_t interval_steps) {
    if (!store)
        return;
    memset(store, 0, sizeof(*store));
    store->auto_checkpoint = auto_checkpoint;
    store->interval_steps = interval_steps;
}

hu_error_t hu_checkpoint_save(hu_checkpoint_store_t *store, hu_allocator_t *alloc,
                              const char *task_id, size_t task_id_len, uint32_t step,
                              hu_checkpoint_status_t status, const char *state_json,
                              size_t state_json_len) {
    if (!store || !task_id)
        return HU_ERR_INVALID_ARGUMENT;

    /* Find existing checkpoint for this task, or allocate new slot */
    hu_checkpoint_t *cp = NULL;
    for (size_t i = 0; i < store->count; i++) {
        if (strlen(store->checkpoints[i].task_id) == task_id_len &&
            memcmp(store->checkpoints[i].task_id, task_id, task_id_len) == 0) {
            cp = &store->checkpoints[i];
            break;
        }
    }
    if (!cp) {
        if (store->count >= HU_CHECKPOINT_MAX_STORED)
            return HU_ERR_OUT_OF_MEMORY;
        cp = &store->checkpoints[store->count++];
        memset(cp, 0, sizeof(*cp));
        int64_t now = (int64_t)time(NULL);
        cp->created_at = now;
        snprintf(cp->id, sizeof(cp->id), "cp-%.*s-%u", (int)(task_id_len < 20 ? task_id_len : 20),
                 task_id, step);
    }

    size_t tid_copy = task_id_len < sizeof(cp->task_id) - 1 ? task_id_len : sizeof(cp->task_id) - 1;
    memcpy(cp->task_id, task_id, tid_copy);
    cp->task_id[tid_copy] = '\0';
    cp->step = step;
    cp->status = status;
    cp->updated_at = (int64_t)time(NULL);

    /* Free previous state if heap-allocated */
    if (cp->state_json && alloc) {
        alloc->free(alloc->ctx, cp->state_json, cp->state_json_len + 1);
        cp->state_json = NULL;
        cp->state_json_len = 0;
    }

    if (state_json && state_json_len > 0 && alloc) {
        cp->state_json = (char *)alloc->alloc(alloc->ctx, state_json_len + 1);
        if (!cp->state_json)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(cp->state_json, state_json, state_json_len);
        cp->state_json[state_json_len] = '\0';
        cp->state_json_len = state_json_len;
    }

    return HU_OK;
}

hu_error_t hu_checkpoint_load(const hu_checkpoint_store_t *store, const char *task_id,
                              size_t task_id_len, hu_checkpoint_t *out) {
    if (!store || !task_id || !out)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = store->count; i > 0; i--) {
        const hu_checkpoint_t *cp = &store->checkpoints[i - 1];
        if (strlen(cp->task_id) == task_id_len && memcmp(cp->task_id, task_id, task_id_len) == 0) {
            *out = *cp;
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
}

hu_error_t hu_checkpoint_load_latest(const hu_checkpoint_store_t *store, hu_checkpoint_t *out) {
    if (!store || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (store->count == 0)
        return HU_ERR_NOT_FOUND;

    const hu_checkpoint_t *latest = &store->checkpoints[0];
    for (size_t i = 1; i < store->count; i++) {
        if (store->checkpoints[i].updated_at > latest->updated_at)
            latest = &store->checkpoints[i];
    }
    *out = *latest;
    return HU_OK;
}

void hu_checkpoint_store_deinit(hu_checkpoint_store_t *store, hu_allocator_t *alloc) {
    if (!store || !alloc)
        return;
    for (size_t i = 0; i < store->count && i < HU_CHECKPOINT_MAX_STORED; i++) {
        hu_checkpoint_t *cp = &store->checkpoints[i];
        if (cp->state_json)
            alloc->free(alloc->ctx, cp->state_json, cp->state_json_len + 1);
        cp->state_json = NULL;
        cp->state_json_len = 0;
    }
    store->count = 0;
}

bool hu_checkpoint_should_save(const hu_checkpoint_store_t *store, uint32_t current_step) {
    if (!store || !store->auto_checkpoint)
        return false;
    if (store->interval_steps == 0)
        return true;
    return (current_step % store->interval_steps) == 0;
}

const char *hu_checkpoint_status_name(hu_checkpoint_status_t status) {
    switch (status) {
    case HU_CHECKPOINT_ACTIVE:
        return "active";
    case HU_CHECKPOINT_PAUSED:
        return "paused";
    case HU_CHECKPOINT_COMPLETED:
        return "completed";
    case HU_CHECKPOINT_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}
