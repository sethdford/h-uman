#ifndef HU_DAEMON_JOB_HOLD_H
#define HU_DAEMON_JOB_HOLD_H

/*
 * Inbound hold while the local model is down (durable job queue, design §5;
 * docs/guides/job-queue.md). The nightly retrain boots mlx-server out for
 * ~70 min. With privacy.local_only there is no cloud fallback, and the
 * chat.db cursor already moved past the message at poll time, so a turn that
 * fails then is a message nobody ever answers. This module decides whether a
 * failed turn's messages are worth holding, and holds them.
 *
 * Gate: HU_JOB_HOLD off|shadow|live (hu_gate_mode_from_env, default OFF),
 * read once at daemon start by hu_daemon_job_queue_start.
 *   OFF     zero work: the failed-turn hook only logs today's line.
 *   SHADOW  on a HOLD verdict logs one line
 *           `[jobq] shadow would hold n=<count> err=<code> probe=down`
 *           and remembers the batch in memory (rowids and timestamps, for the
 *           release-side shadow lines). Nothing is written; nothing changes.
 *   LIVE    enqueues each message of the batch as an `inbound_hold` job,
 *           key hold:<chat_id>:<rowid>. Needs the jobs table, so it also needs
 *           HU_JOB_QUEUE=shadow|live; without a started queue it runs as
 *           SHADOW (logged once at start).
 *
 * Only the iMessage channel is held (vtable name "imessage"): it is the
 * channel whose cursor loses the message, and the release side asks chat.db
 * whether the owner already answered.
 */

#include "human/channel_loop.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_agent;
struct hu_config;
struct hu_service_channel;

#define HU_JOB_HOLD_ENV          "HU_JOB_HOLD"
#define HU_JOB_HOLD_CHANNEL      "imessage"
#define HU_JOB_HOLD_MAX_AGE_S    (3 * 3600) /* HU_TRAIN_WINDOW 02:00-05:00; outage ~70 min */
#define HU_JOB_HOLD_SHADOW_SLOTS 64

/* hu_mlx_admin_probe_health, as the hold decision sees it. UNKNOWN: no
 * mlx_local provider configured, so "down" cannot be told apart from "not
 * the provider in use". */
typedef enum hu_job_probe {
    HU_JOB_PROBE_UNKNOWN = 0,
    HU_JOB_PROBE_UP,
    HU_JOB_PROBE_DOWN,
} hu_job_probe_t;

typedef enum hu_job_turn_verdict {
    HU_JOB_TURN_ONE_OFF = 0, /* today's behaviour: logged, nothing held */
    HU_JOB_TURN_HOLD,
} hu_job_turn_verdict_t;

/* Pure: HOLD only when the turn failed with a transport error
 * (hu_agent_error_is_transport) AND the model probe says DOWN. A transport
 * error with the probe up or unknown is a one-off; any other error (or
 * HU_OK) is ONE_OFF. */
hu_job_turn_verdict_t hu_job_hold_decide(hu_error_t turn_err, hu_job_probe_t probe);

/* Serialise what re-injecting `m` needs, except session_key (stored in the
 * job's contact column). HU_ERR_INVALID_ARGUMENT when the result would not
 * fit in `cap` (callers pass HU_JOB_PAYLOAD_MAX: such a message is not held). */
hu_error_t hu_job_hold_encode(const hu_channel_loop_msg_t *m, unsigned char *buf, size_t cap,
                              size_t *out_len);

/* Inverse of hu_job_hold_encode. session_key comes from the job's contact
 * column. HU_ERR_PARSE on a truncated or unknown-version payload. */
hu_error_t hu_job_hold_decode(const unsigned char *buf, size_t len, const char *session_key,
                              hu_channel_loop_msg_t *out);

typedef struct hu_daemon_job_hold_metrics {
    hu_gate_mode_t mode;         /* effective mode set at start */
    uint64_t transport_one_offs; /* transport error, probe up/unknown */
    uint64_t shadow_would_hold;  /* messages a SHADOW hold would have held */
    uint64_t held;               /* LIVE: jobs inserted */
    uint64_t hold_duplicates;    /* LIVE: key already present (never re-held) */
    uint64_t hold_skipped;       /* LIVE: too large / no rowid / enqueue error */
    uint64_t released;           /* LIVE: re-injected into a poll batch */
    uint64_t canceled;           /* LIVE: owner had already answered */
    uint64_t expired;            /* LIVE: held longer than HU_JOB_HOLD_MAX_AGE_S */
    uint64_t expiry_notices;     /* LIVE: owner notifications (one per expiry batch) */
    uint64_t release_failures;   /* LIVE: undecodable payload / transition refused */
    uint64_t shadow_would_release;
    uint64_t shadow_would_cancel;
    uint64_t shadow_would_expire;
} hu_daemon_job_hold_metrics_t;

hu_gate_mode_t hu_daemon_job_hold_mode(void);
void hu_daemon_job_hold_metrics(hu_daemon_job_hold_metrics_t *out);
void hu_daemon_job_hold_reset_for_test(void);
/* Tests only: pin the clock the hold/release sides read (0 = wall clock). */
void hu_daemon_job_hold_set_now_for_test(int64_t now);

/* The daemon's failed-turn hook: logs today's "agent turn failed for <who>"
 * line, then (only for a transport error on the iMessage channel, gate not
 * OFF) probes the model and applies the verdict to msgs[batch_start..
 * batch_end]. `config` supplies the mlx_local base URL for the probe. */
void hu_daemon_jobs_on_turn_error(struct hu_agent *agent, const struct hu_config *config,
                                  const struct hu_service_channel *ch,
                                  const hu_channel_loop_msg_t *msgs, size_t batch_start,
                                  size_t batch_end, hu_error_t err);

/* The daemon's poll call (hu_service_run): runs ch->poll_fn exactly as
 * before, then, on the iMessage channel with the gate not OFF, at most every
 * 30 s:
 *   LIVE    expires held rows older than HU_JOB_HOLD_MAX_AGE_S (one owner
 *           notification per expiry batch); if the model probe is UP, claims
 *           due inbound_hold rows that fit in max_msgs, cancels any the owner
 *           already answered, and puts the rest BEFORE the polled messages
 *           (oldest first) so they take the normal turn. Each row goes
 *           claimed -> sending -> done before this returns, i.e. before any
 *           turn runs: a held message is handed to a turn at most once.
 *   SHADOW  logs `would expire n` / `would release n age_max=` /
 *           `would cancel n` for the in-memory would-holds; msgs unchanged.
 * OFF returns poll_fn's result and output untouched. */
hu_error_t hu_daemon_jobs_poll(struct hu_service_channel *ch, hu_allocator_t *alloc,
                               struct hu_agent *agent, const struct hu_config *config,
                               hu_channel_loop_msg_t *msgs, size_t max_msgs, size_t *out_count);

/* Did the owner answer this conversation after `rowid`? Default: chat.db via
 * hu_imessage_channel_replied_after (macOS iMessage builds; false elsewhere
 * and in tests). Tests inject a stub; NULL restores the default. */
typedef bool (*hu_job_hold_replied_fn)(void *channel_ctx, const char *chat_id, const char *handle,
                                       int64_t rowid);
void hu_daemon_job_hold_set_replied_fn_for_test(hu_job_hold_replied_fn fn);

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
/* Called by hu_daemon_job_queue_start: the effective mode and the jobs
 * handle (NULL unless the queue started). */
void hu_daemon_job_hold_configure(hu_gate_mode_t mode, sqlite3 *db);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_JOB_HOLD_H */
