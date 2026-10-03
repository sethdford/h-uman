#ifndef HU_DAEMON_JOB_QUEUE_H
#define HU_DAEMON_JOB_QUEUE_H

/*
 * Daemon side of the durable job queue (docs/plans/2026-10-03-durable-job-queue.md
 * §2, PR 2): the gate, start-of-process recovery, and the counters later PRs
 * (hold path, scheduled-send migration) report through. The table itself is
 * include/human/memory/job_queue_repo.h.
 *
 * Gate: HU_JOB_QUEUE off|shadow|live (hu_gate_mode_from_env, default OFF).
 *   OFF     start logs one line naming the mode and touches nothing: no
 *           schema, no query.
 *   SHADOW  start creates the table if missing, runs recover_on_start, and
 *   LIVE    logs one aggregate line of counts (never payloads or handles).
 * Nothing in this module changes what is sent or replied, in any mode: no
 * producer or consumer uses the queue yet. SHADOW -> LIVE for the sends that
 * will move onto it is gated on the §9 measurement (14 days of shadow
 * agreement plus a kill -9 drill showing no re-send), not on this module.
 */

#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include "human/daemon/job_hold.h" /* inbound hold: hu_service_run hooks */
#include "human/memory/job_queue_repo.h"
#include "human/observer.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hu_daemon_job_queue_metrics {
    hu_gate_mode_t mode;            /* mode the last start ran under */
    bool started;                   /* schema ensured and recovery ran */
    int64_t recovered_unknown;      /* sending -> unknown at the last start */
    int64_t recovered_requeued;     /* expired-lease claimed -> pending */
    hu_job_queue_counts_t at_start; /* row counts after recovery */
    int64_t start_failures;         /* starts whose schema/recovery/count failed */
} hu_daemon_job_queue_metrics_t;

/* getenv("HU_JOB_QUEUE") through hu_gate_mode_from_env; unset -> OFF. */
hu_gate_mode_t hu_daemon_job_queue_mode(void);

/* Snapshot of the counters (zeroed until a start ran). */
void hu_daemon_job_queue_metrics(hu_daemon_job_queue_metrics_t *out);

/* Tests only: forget the last start. */
void hu_daemon_job_queue_reset_for_test(void);

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

/* Daemon start (once, before the poll loop). Reads the gate, logs one line
 * naming it, and in SHADOW/LIVE ensures the schema, recovers rows left
 * in flight by the previous process, and logs one count line. OFF returns
 * HU_OK without touching `db`. A NULL `db` outside OFF is
 * HU_ERR_INVALID_ARGUMENT (logged; the daemon carries on without a queue). */
hu_error_t hu_daemon_job_queue_start(sqlite3 *db, hu_observer_t *obs);
#endif

#ifdef __cplusplus
}
#endif

#endif /* HU_DAEMON_JOB_QUEUE_H */
