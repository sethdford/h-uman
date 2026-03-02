#ifndef SC_STATE_H
#define SC_STATE_H

#include "core/allocator.h"
#include "core/error.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Process state — starting, running, stopping
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum sc_process_state {
    SC_PROCESS_STATE_STARTING,
    SC_PROCESS_STATE_RUNNING,
    SC_PROCESS_STATE_STOPPING,
    SC_PROCESS_STATE_STOPPED,
} sc_process_state_t;

/* ──────────────────────────────────────────────────────────────────────────
 * State manager — thread-safe process state + optional persistence
 * ────────────────────────────────────────────────────────────────────────── */

#define SC_STATE_CHANNEL_LEN 64
#define SC_STATE_CHAT_ID_LEN 128

typedef struct sc_state_data {
    char last_channel[SC_STATE_CHANNEL_LEN];
    char last_chat_id[SC_STATE_CHAT_ID_LEN];
    int64_t updated_at;
} sc_state_data_t;

typedef struct sc_state_manager {
    sc_allocator_t *alloc;
    sc_process_state_t process_state;
    sc_state_data_t data;
    char *state_path;  /* owned, for persistence; NULL if none */
} sc_state_manager_t;

/* Initialize. state_path may be NULL (no persistence). */
sc_error_t sc_state_manager_init(sc_state_manager_t *mgr,
                                 sc_allocator_t *alloc,
                                 const char *state_path);

/* Free resources. */
void sc_state_manager_deinit(sc_state_manager_t *mgr);

/* Set process state. */
void sc_state_set_process(sc_state_manager_t *mgr, sc_process_state_t state);

/* Get process state. */
sc_process_state_t sc_state_get_process(const sc_state_manager_t *mgr);

/* Set last active channel/chat (for heartbeat routing). */
void sc_state_set_last_channel(sc_state_manager_t *mgr,
                                const char *channel,
                                const char *chat_id);

/* Get last channel/chat. */
void sc_state_get_last_channel(const sc_state_manager_t *mgr,
                                char *channel_out,
                                size_t channel_size,
                                char *chat_id_out,
                                size_t chat_id_size);

/* Save to disk (if state_path set). */
sc_error_t sc_state_save(sc_state_manager_t *mgr);

/* Load from disk (if state_path set). */
sc_error_t sc_state_load(sc_state_manager_t *mgr);

/* Default state path: workspace_dir/state.json */
char *sc_state_default_path(sc_allocator_t *alloc, const char *workspace_dir);

#endif /* SC_STATE_H */
