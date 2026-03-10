#ifndef HU_STATE_H
#define HU_STATE_H

#include "core/allocator.h"
#include "core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Process state — starting, running, stopping
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_process_state {
    HU_PROCESS_STATE_STARTING,
    HU_PROCESS_STATE_RUNNING,
    HU_PROCESS_STATE_STOPPING,
    HU_PROCESS_STATE_STOPPED,
} hu_process_state_t;

/* ──────────────────────────────────────────────────────────────────────────
 * State manager — thread-safe process state + optional persistence
 * ────────────────────────────────────────────────────────────────────────── */

#define HU_STATE_CHANNEL_LEN 64
#define HU_STATE_CHAT_ID_LEN 128

typedef struct hu_state_data {
    char last_channel[HU_STATE_CHANNEL_LEN];
    char last_chat_id[HU_STATE_CHAT_ID_LEN];
    int64_t updated_at;
} hu_state_data_t;

typedef struct hu_state_manager {
    hu_allocator_t *alloc;
    hu_process_state_t process_state;
    hu_state_data_t data;
    char *state_path; /* owned, for persistence; NULL if none */
} hu_state_manager_t;

/* Initialize. state_path may be NULL (no persistence). */
hu_error_t hu_state_manager_init(hu_state_manager_t *mgr, hu_allocator_t *alloc,
                                 const char *state_path);

/* Free resources. */
void hu_state_manager_deinit(hu_state_manager_t *mgr);

/* Set process state. */
void hu_state_set_process(hu_state_manager_t *mgr, hu_process_state_t state);

/* Get process state. */
hu_process_state_t hu_state_get_process(const hu_state_manager_t *mgr);

/* Set last active channel/chat (for heartbeat routing). */
void hu_state_set_last_channel(hu_state_manager_t *mgr, const char *channel, const char *chat_id);

/* Get last channel/chat. */
void hu_state_get_last_channel(const hu_state_manager_t *mgr, char *channel_out,
                               size_t channel_size, char *chat_id_out, size_t chat_id_size);

/* Save to disk (if state_path set). */
hu_error_t hu_state_save(hu_state_manager_t *mgr);

/* Load from disk (if state_path set). */
hu_error_t hu_state_load(hu_state_manager_t *mgr);

/* Default state path: workspace_dir/state.json */
char *hu_state_default_path(hu_allocator_t *alloc, const char *workspace_dir);

#endif /* HU_STATE_H */
