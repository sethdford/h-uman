#ifndef HU_DAEMON_JOB_HOLD_INTERNAL_H
#define HU_DAEMON_JOB_HOLD_INTERNAL_H

/* Private to daemon_job_hold.c (the hold side) and daemon_job_release.c
 * (the release side): the state they share. Public contract:
 * include/human/daemon/job_hold.h. */

#include "human/daemon/job_hold.h"

struct hu_service_channel;

/* A SHADOW would-hold, remembered in memory only so the release side can
 * log what it would have released, canceled or expired. Never logged. */
typedef struct hu_job_hold_shadow_entry {
    bool used;
    int64_t rowid;
    int64_t held_at; /* unix seconds the turn failed */
    char handle[sizeof(((hu_channel_loop_msg_t *)0)->session_key)];
    char chat_id[sizeof(((hu_channel_loop_msg_t *)0)->chat_id)];
} hu_job_hold_shadow_entry_t;

/* Wall-clock seconds, or the test clock when one is set. */
int64_t hu_job_hold_now(void);

/* hu_mlx_admin_probe_health against config's mlx_local base URL. */
hu_job_probe_t hu_job_hold_probe(const struct hu_config *config);

/* The channel's vtable name is HU_JOB_HOLD_CHANNEL. */
bool hu_job_hold_is_imessage(const struct hu_service_channel *ch);

#endif /* HU_DAEMON_JOB_HOLD_INTERNAL_H */
