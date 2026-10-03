/* Durable job queue, daemon side: gate, start recovery, counters.
 * Contract: include/human/daemon/job_queue.h. */
#include "human/daemon/job_queue.h"

#include "human/core/log.h"
#include <string.h>

#define JOB_QUEUE_ENV "HU_JOB_QUEUE"

/* Written once at daemon start (single-threaded, before the poll loop) and
 * read afterwards; later PRs that bump counters from the loop must make the
 * fields they touch atomic. */
static hu_daemon_job_queue_metrics_t g_jobq;

hu_gate_mode_t hu_daemon_job_queue_mode(void) {
    return hu_gate_mode_from_env(JOB_QUEUE_ENV, HU_GATE_OFF);
}

void hu_daemon_job_queue_metrics(hu_daemon_job_queue_metrics_t *out) {
    if (out)
        *out = g_jobq;
}

void hu_daemon_job_queue_reset_for_test(void) {
    memset(&g_jobq, 0, sizeof(g_jobq));
}

#ifdef HU_ENABLE_SQLITE

#include "human/core/time.h"

static const char *jobq_mode_name(hu_gate_mode_t mode) {
    return mode == HU_GATE_LIVE ? "live" : mode == HU_GATE_SHADOW ? "shadow" : "off";
}

/* Schema, recovery, counts. Fills g_jobq; returns the first failure. */
static hu_error_t jobq_recover(sqlite3 *db, int64_t now_s) {
    hu_error_t err = hu_job_queue_repo_ensure_schema(db);
    if (err == HU_OK)
        err = hu_job_queue_repo_recover_on_start(db, now_s, &g_jobq.recovered_unknown,
                                                 &g_jobq.recovered_requeued);
    if (err == HU_OK)
        err = hu_job_queue_repo_counts(db, &g_jobq.at_start);
    return err;
}

hu_error_t hu_daemon_job_queue_start(sqlite3 *db, hu_observer_t *obs) {
    hu_gate_mode_t mode = hu_daemon_job_queue_mode();
    g_jobq.mode = mode;
    g_jobq.started = false;
    g_jobq.recovered_unknown = 0;
    g_jobq.recovered_requeued = 0;
    memset(&g_jobq.at_start, 0, sizeof(g_jobq.at_start));
    if (mode == HU_GATE_OFF) {
        hu_log_info("jobq", obs,
                    "[jobq] durable job queue off (HU_JOB_QUEUE unset/off); set "
                    "HU_JOB_QUEUE=shadow to create the jobs table and run start recovery");
        return HU_OK;
    }
    const char *name = jobq_mode_name(mode);
    hu_log_info("jobq", obs, "[jobq] durable job queue HU_JOB_QUEUE=%s", name);
    hu_error_t err = db ? jobq_recover(db, hu_time_wall_ms() / 1000) : HU_ERR_INVALID_ARGUMENT;
    if (err != HU_OK) {
        g_jobq.start_failures++;
        hu_log_warn("jobq", obs,
                    "[jobq %s] start recovery failed (err=%d); the queue is unavailable this run",
                    name, (int)err);
        return err;
    }
    g_jobq.started = true;
    const hu_job_queue_counts_t *c = &g_jobq.at_start;
    /* Counts only: never payloads, contacts or keys. */
    hu_log_info("jobq", obs,
                "[jobq %s] start: recovered unknown=%lld requeued=%lld | pending=%lld "
                "claimed=%lld sending=%lld unknown=%lld done=%lld failed=%lld expired=%lld "
                "canceled=%lld shadow=%lld",
                name, (long long)g_jobq.recovered_unknown, (long long)g_jobq.recovered_requeued,
                (long long)c->pending, (long long)c->claimed, (long long)c->sending,
                (long long)c->unknown, (long long)c->done, (long long)c->failed,
                (long long)c->expired, (long long)c->canceled, (long long)c->shadow);
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */
