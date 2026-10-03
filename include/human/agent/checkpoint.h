#ifndef HU_AGENT_CHECKPOINT_H
#define HU_AGENT_CHECKPOINT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Checkpoint/resume for long-running agent tasks (LangGraph-style).
 * Serializes agent state at configurable intervals so tasks can be
 * resumed after interruption, crash, or timeout.
 */

typedef enum hu_checkpoint_status {
    HU_CHECKPOINT_ACTIVE = 0,
    HU_CHECKPOINT_PAUSED,
    HU_CHECKPOINT_COMPLETED,
    HU_CHECKPOINT_FAILED,
} hu_checkpoint_status_t;

typedef struct hu_checkpoint {
    char id[64];
    char task_id[64];
    uint32_t step;
    hu_checkpoint_status_t status;
    int64_t created_at; /* unix timestamp */
    int64_t updated_at;
    char *state_json; /* serialized agent state */
    size_t state_json_len;
    char *metadata_json; /* user-defined metadata */
    size_t metadata_json_len;
} hu_checkpoint_t;

#define HU_CHECKPOINT_MAX_STORED 64

typedef struct hu_checkpoint_store {
    hu_checkpoint_t checkpoints[HU_CHECKPOINT_MAX_STORED];
    size_t count;
    bool auto_checkpoint;    /* checkpoint after each tool call */
    uint32_t interval_steps; /* checkpoint every N steps; 0 = every step */
} hu_checkpoint_store_t;

void hu_checkpoint_store_init(hu_checkpoint_store_t *store, bool auto_checkpoint,
                              uint32_t interval_steps);

hu_error_t hu_checkpoint_save(hu_checkpoint_store_t *store, hu_allocator_t *alloc,
                              const char *task_id, size_t task_id_len, uint32_t step,
                              hu_checkpoint_status_t status, const char *state_json,
                              size_t state_json_len);

hu_error_t hu_checkpoint_load(const hu_checkpoint_store_t *store, const char *task_id,
                              size_t task_id_len, hu_checkpoint_t *out);

hu_error_t hu_checkpoint_load_latest(const hu_checkpoint_store_t *store, hu_checkpoint_t *out);

/* Frees every slot's state_json (the copies hu_checkpoint_save allocated with
 * `alloc`) and empties the store; auto_checkpoint and interval_steps are kept.
 * metadata_json is never allocated by the store and is left alone. NULL-safe. */
void hu_checkpoint_store_deinit(hu_checkpoint_store_t *store, hu_allocator_t *alloc);

bool hu_checkpoint_should_save(const hu_checkpoint_store_t *store, uint32_t current_step);

const char *hu_checkpoint_status_name(hu_checkpoint_status_t status);

#endif /* HU_AGENT_CHECKPOINT_H */
