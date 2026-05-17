#include "human/agent/scheduler.h"
#include "human/core/log.h"

#include "human/agent/autodream.h"
#include "human/core/log.h"
#include "human/memory/memory.h"
#include "human/persona/persona_deltas.h"

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* DONE(W14-daemon FIX 13): `hu_scheduler_tick` is now wired into the
 * once-per-minute branch of src/daemon.c via the W14 bridge in
 * src/agent/world_model_bridge.c.  See the FIX 13 wire-up comment in
 * daemon.c near the autodream cron block. */

#define HU_SCHED_MAX_JOBS_PER_TICK     32
#define HU_SCHED_TOTAL_BUDGET_MS       10000
#define HU_SCHED_DEFAULT_JOB_BUDGET_MS 60000
#define HU_SCHED_IDLE_LOAD_MAX         60
#define HU_SCHED_EMERGENCY_PRIORITY    2

typedef struct runner_slot {
    hu_job_runner_fn fn;
    void *user_data;
} runner_slot_t;

struct hu_scheduler {
    hu_allocator_t *alloc;
    hu_memory_facade_t *m;
    runner_slot_t runners[HU_JOB_KIND_MAX];
    hu_persona_t *persona;
};

#ifdef HU_ENABLE_SQLITE
/* Wall clock (monotonic-ish) in milliseconds. Only referenced from the
 * SQLite-enabled scheduler paths below; gate the definition so the
 * no-sqlite / minimal build doesn't trip -Werror=unused-function. */
static int64_t scheduler_now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return (int64_t)time(NULL) * 1000;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}
#endif

/* ── Default runners ─────────────────────────────────────────────────── */

static hu_error_t default_noop_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec,
                                      int64_t budget_ms, void *user_data) {
    (void)m;
    (void)spec;
    (void)budget_ms;
    (void)user_data;
    return HU_OK;
}

#ifdef HU_ENABLE_SQLITE
/* Shared autodream wrapper.  Runs `hu_autodream_run_on_facade` with selective
 * phase enables so each AutoDream kind owns its slice of consolidation work
 * without duplicating logic.  The W14 spec is explicit: WRAP existing
 * autodream.c functions, do not reimplement.
 *
 * Allocator: runners do not receive an allocator through the vtable, so
 * we use the system allocator here. AutoDream's transient buffers are
 * still tracked by ASan because the system allocator is what every
 * other agent-layer module also passes.
 */
static hu_error_t run_autodream_phase(hu_memory_facade_t *m, int64_t budget_ms, bool quarantine,
                                      bool communities, bool reweight, bool derived) {
    if (!m)
        return HU_OK;
    if (!hu_memory_facade_graph_handle(m))
        return HU_OK;
    hu_autodream_config_t cfg = hu_autodream_default_config();
    cfg.enable_quarantine_review = quarantine;
    cfg.enable_community_summaries = communities;
    cfg.enable_edge_reweight = reweight;
    cfg.enable_derived_facts = derived;
    if (budget_ms > 0)
        cfg.max_runtime_ms = budget_ms;
    hu_autodream_report_t rep = {0};
    hu_allocator_t sys = hu_system_allocator();
    return hu_autodream_run_on_facade(&sys, m, &cfg, &rep);
}

static hu_error_t default_autodream_quarantine_runner(hu_memory_facade_t *m,
                                                      const hu_job_spec_t *spec, int64_t budget_ms,
                                                      void *user_data) {
    (void)spec;
    (void)user_data;
    return run_autodream_phase(m, budget_ms, true, false, false, false);
}

static hu_error_t default_autodream_community_runner(hu_memory_facade_t *m,
                                                     const hu_job_spec_t *spec, int64_t budget_ms,
                                                     void *user_data) {
    (void)spec;
    (void)user_data;
    return run_autodream_phase(m, budget_ms, false, true, false, false);
}

static hu_error_t default_autodream_decay_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec,
                                                 int64_t budget_ms, void *user_data) {
    (void)spec;
    (void)user_data;
    return run_autodream_phase(m, budget_ms, false, false, true, true);
}

static hu_error_t default_persona_evolver_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec,
                                                 int64_t budget_ms, void *user_data) {
    (void)user_data;
    if (!m)
        return HU_OK;
    hu_persona_evolver_config_t cfg = hu_persona_evolver_default_config();
    /* persona evolver has no wall-clock budget field; it bounds work via
     * `max_apply` and `rate_limit_per_hour`. The scheduler budget is advisory. */
    (void)budget_ms;
    cfg.now_ms = (int64_t)time(NULL) * 1000;
    const char *cid = (spec && spec->contact_id) ? spec->contact_id : "";
    size_t cid_len = (spec && spec->contact_id) ? spec->contact_id_len : 0;
    hu_persona_evolver_report_t rep = {0};
    return hu_persona_evolver_run_facade(m, cid, cid_len, &cfg, &rep);
}
#else
static hu_error_t default_autodream_quarantine_runner(hu_memory_facade_t *m,
                                                      const hu_job_spec_t *spec, int64_t budget_ms,
                                                      void *user_data) {
    (void)m;
    (void)spec;
    (void)budget_ms;
    (void)user_data;
    return HU_OK;
}
static hu_error_t default_autodream_community_runner(hu_memory_facade_t *m,
                                                     const hu_job_spec_t *spec, int64_t budget_ms,
                                                     void *user_data) {
    (void)m;
    (void)spec;
    (void)budget_ms;
    (void)user_data;
    return HU_OK;
}
static hu_error_t default_autodream_decay_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec,
                                                 int64_t budget_ms, void *user_data) {
    (void)m;
    (void)spec;
    (void)budget_ms;
    (void)user_data;
    return HU_OK;
}
static hu_error_t default_persona_evolver_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec,
                                                 int64_t budget_ms, void *user_data) {
    (void)m;
    (void)spec;
    (void)budget_ms;
    (void)user_data;
    return HU_OK;
}
#endif /* HU_ENABLE_SQLITE */

static void install_default_runners(hu_scheduler_t *s) {
    for (size_t i = 0; i < HU_JOB_KIND_MAX; i++) {
        s->runners[i].fn = default_noop_runner;
        s->runners[i].user_data = NULL;
    }
    s->runners[HU_JOB_AUTODREAM_QUARANTINE].fn = default_autodream_quarantine_runner;
    s->runners[HU_JOB_AUTODREAM_COMMUNITY].fn = default_autodream_community_runner;
    s->runners[HU_JOB_AUTODREAM_DECAY].fn = default_autodream_decay_runner;
    s->runners[HU_JOB_PERSONA_EVOLVER].fn = default_persona_evolver_runner;
}

#ifdef HU_ENABLE_SQLITE

/* ── Schema ──────────────────────────────────────────────────────────── */

static int run_ddl(struct sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &st, NULL);
    if (rc != SQLITE_OK)
        return rc;
    rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return (rc == SQLITE_DONE || rc == SQLITE_OK) ? SQLITE_OK : rc;
}

static hu_error_t ensure_schema(struct sqlite3 *db) {
    static const char *const stmts[] = {
        "CREATE TABLE IF NOT EXISTS scheduler_jobs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "kind INTEGER NOT NULL,"
        "contact_id TEXT NOT NULL DEFAULT '',"
        "priority INTEGER NOT NULL DEFAULT 0,"
        "budget_ms INTEGER NOT NULL,"
        "interval_sec INTEGER NOT NULL DEFAULT 0,"
        "earliest_at INTEGER NOT NULL DEFAULT 0,"
        "latest_at INTEGER NOT NULL DEFAULT 0,"
        "requires_idle INTEGER NOT NULL DEFAULT 1,"
        "requires_ac_power INTEGER NOT NULL DEFAULT 0,"
        "last_run_at INTEGER NOT NULL DEFAULT 0,"
        "status TEXT NOT NULL DEFAULT 'pending',"
        "last_error TEXT)",

        "CREATE INDEX IF NOT EXISTS idx_scheduler_jobs_pending "
        "ON scheduler_jobs(status, priority, earliest_at)",

        NULL,
    };
    for (size_t i = 0; stmts[i]; i++) {
        if (run_ddl(db, stmts[i]) != SQLITE_OK)
            return HU_ERR_IO;
    }
    return HU_OK;
}

static struct sqlite3 *get_db(hu_memory_facade_t *m) {
    return hu_memory_facade_sqlite_db(m);
}

#endif /* HU_ENABLE_SQLITE */

/* ── Lifecycle ───────────────────────────────────────────────────────── */

hu_error_t hu_scheduler_open(hu_allocator_t *alloc, hu_memory_facade_t *m, hu_scheduler_t **out) {
    if (!alloc || !m || !out)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;

#ifdef HU_ENABLE_SQLITE
    struct sqlite3 *db = get_db(m);
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t rc = ensure_schema(db);
    if (rc != HU_OK)
        return rc;
#else
    return HU_ERR_NOT_SUPPORTED;
#endif

    hu_scheduler_t *s = alloc->alloc(alloc->ctx, sizeof(*s));
    if (!s)
        return HU_ERR_OUT_OF_MEMORY;
    memset(s, 0, sizeof(*s));
    s->alloc = alloc;
    s->m = m;
    install_default_runners(s);
    *out = s;
    return HU_OK;
}

void hu_scheduler_close(hu_scheduler_t *s, hu_allocator_t *alloc) {
    if (!s)
        return;
    hu_allocator_t *a = alloc ? alloc : s->alloc;
    if (a)
        a->free(a->ctx, s, sizeof(*s));
}

hu_error_t hu_scheduler_register_runner(hu_scheduler_t *s, hu_job_kind_t kind, hu_job_runner_fn fn,
                                        void *user_data) {
    if (!s)
        return HU_ERR_INVALID_ARGUMENT;
    if ((int)kind < 0 || kind >= HU_JOB_KIND_MAX)
        return HU_ERR_INVALID_ARGUMENT;
    s->runners[kind].fn = fn ? fn : default_noop_runner;
    s->runners[kind].user_data = fn ? user_data : NULL;
    return HU_OK;
}

void hu_scheduler_set_persona(hu_scheduler_t *s, hu_persona_t *p) {
    if (s)
        s->persona = p;
}

/* ── Enqueue ─────────────────────────────────────────────────────────── */

#ifdef HU_ENABLE_SQLITE

hu_error_t hu_scheduler_enqueue(hu_scheduler_t *s, const hu_job_spec_t *job) {
    if (!s || !job)
        return HU_ERR_INVALID_ARGUMENT;
    if ((int)job->kind < 0 || job->kind >= HU_JOB_KIND_MAX)
        return HU_ERR_INVALID_ARGUMENT;
    if (job->budget_ms < 0 || job->interval_sec < 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (job->earliest_at < 0 || job->latest_at < 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (job->latest_at > 0 && job->earliest_at > 0 && job->latest_at < job->earliest_at)
        return HU_ERR_INVALID_ARGUMENT;

    struct sqlite3 *db = get_db(s->m);
    if (!db)
        return HU_ERR_IO;

    static const char *const sql =
        "INSERT INTO scheduler_jobs ("
        "kind, contact_id, priority, budget_ms, interval_sec, "
        "earliest_at, latest_at, requires_idle, requires_ac_power, status) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, 'pending')";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    const char *cid = job->contact_id ? job->contact_id : "";
    int cid_len = (int)(job->contact_id ? job->contact_id_len : 0);
    int budget = job->budget_ms > 0 ? job->budget_ms : HU_SCHED_DEFAULT_JOB_BUDGET_MS;

    sqlite3_bind_int(st, 1, (int)job->kind);
    sqlite3_bind_text(st, 2, cid, cid_len, SQLITE_STATIC);
    sqlite3_bind_int(st, 3, job->priority);
    sqlite3_bind_int(st, 4, budget);
    sqlite3_bind_int(st, 5, job->interval_sec);
    sqlite3_bind_int64(st, 6, job->earliest_at);
    sqlite3_bind_int64(st, 7, job->latest_at);
    sqlite3_bind_int(st, 8, job->requires_idle ? 1 : 0);
    sqlite3_bind_int(st, 9, job->requires_ac_power ? 1 : 0);

    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? HU_OK : HU_ERR_IO;
}

/* ── Tick loop ───────────────────────────────────────────────────────── */

typedef struct dispatch_row {
    int64_t id;
    int kind;
    int priority;
    int budget_ms;
    int interval_sec;
    int requires_idle;
    int requires_ac_power;
    int64_t latest_at;
    /* 2026-05-16 P3-5: contact scope.  Empty string = global job. */
    char contact_id[64];
    size_t contact_id_len;
} dispatch_row_t;

static void mark_expired(struct sqlite3 *db, int64_t now_ms) {
    sqlite3_stmt *st = NULL;
    static const char *const sql = "UPDATE scheduler_jobs SET status = 'expired' "
                                   "WHERE status = 'pending' AND latest_at > 0 AND latest_at < ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return;
    sqlite3_bind_int64(st, 1, now_ms);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static int update_status(struct sqlite3 *db, int64_t job_id, const char *status, const char *err,
                         int64_t last_run_ms, int interval_sec, int64_t now_ms) {
    /* Repeating jobs (interval_sec > 0) re-enter the queue with
     * status='pending' and a new earliest_at. One-shot jobs land on the
     * terminal status. */
    sqlite3_stmt *st = NULL;
    if (interval_sec > 0 && strcmp(status, "done") == 0) {
        static const char *const sql =
            "UPDATE scheduler_jobs SET status = 'pending', last_run_at = ?, "
            "last_error = NULL, earliest_at = ? WHERE id = ?";
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
            return -1;
        sqlite3_bind_int64(st, 1, last_run_ms);
        sqlite3_bind_int64(st, 2, now_ms + (int64_t)interval_sec * 1000);
        sqlite3_bind_int64(st, 3, job_id);
    } else {
        static const char *const sql = "UPDATE scheduler_jobs SET status = ?, last_run_at = ?, "
                                       "last_error = ? WHERE id = ?";
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
            return -1;
        sqlite3_bind_text(st, 1, status, -1, SQLITE_STATIC);
        sqlite3_bind_int64(st, 2, last_run_ms);
        if (err && err[0])
            sqlite3_bind_text(st, 3, err, -1, SQLITE_STATIC);
        else
            sqlite3_bind_null(st, 3);
        sqlite3_bind_int64(st, 4, job_id);
    }
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int set_running(struct sqlite3 *db, int64_t job_id) {
    sqlite3_stmt *st = NULL;
    static const char *const sql = "UPDATE scheduler_jobs SET status = 'running' WHERE id = ?";
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_int64(st, 1, job_id);
    int rc = sqlite3_step(st);
    sqlite3_finalize(st);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* Decide whether a job is allowed to run under current system state.
 * `*reason_out` receives a short human-readable reason when blocked. */
static bool job_eligible(const dispatch_row_t *row, int load_pct, int battery_pct, bool on_ac,
                         bool quiet, const char **reason_out) {
    (void)battery_pct;
    if (quiet && row->priority < HU_SCHED_EMERGENCY_PRIORITY) {
        *reason_out = "quiet hours";
        return false;
    }
    if (row->requires_ac_power && !on_ac) {
        *reason_out = "on battery";
        return false;
    }
    if (row->requires_idle && load_pct > HU_SCHED_IDLE_LOAD_MAX) {
        *reason_out = "system busy";
        return false;
    }
    *reason_out = NULL;
    return true;
}

static hu_error_t scheduler_tick_impl(hu_scheduler_t *s, int64_t now_ms,
                                      const char *target_contact_id, size_t target_contact_id_len,
                                      bool scoped);

hu_error_t hu_scheduler_tick(hu_scheduler_t *s, int64_t now_ms) {
    /* Legacy entrypoint: drains every pending job regardless of contact_id.
     * Used by the daemon-wide background scheduler for tasks like autodream
     * phases, LoRA training, and counterfactual rehearsal — none of which
     * fire outbound user messages, and all of which carry their own
     * contact_id forward through `hu_job_spec_t` to the runner.
     *
     * 2026-05-16 P3-5: callers that DO drive outbound user-facing work
     * (proactive sends, per-contact dispatch decisions) MUST use
     * `hu_scheduler_tick_for(s, now, contact_id, len)` so that a job
     * enqueued for "alice" cannot fire on "bob"'s turn. */
    return scheduler_tick_impl(s, now_ms, NULL, 0, /*scoped=*/false);
}

hu_error_t hu_scheduler_tick_for(hu_scheduler_t *s, int64_t now_ms, const char *target_contact_id,
                                 size_t target_contact_id_len) {
    /* Scoped variant — see header doc.  When target is NULL/"", dispatch
     * only contact_id='' rows.  When non-empty, dispatch contact_id=''
     * OR contact_id=target (exact match, no prefix). */
    return scheduler_tick_impl(s, now_ms, target_contact_id, target_contact_id_len,
                               /*scoped=*/true);
}

static hu_error_t scheduler_tick_impl(hu_scheduler_t *s, int64_t now_ms,
                                      const char *target_contact_id, size_t target_contact_id_len,
                                      bool scoped) {
    if (!s)
        return HU_ERR_INVALID_ARGUMENT;
    struct sqlite3 *db = get_db(s->m);
    if (!db)
        return HU_ERR_IO;

    if (now_ms <= 0)
        now_ms = (int64_t)time(NULL) * 1000;

    mark_expired(db, now_ms);

    int load_pct = hu_scheduler_probe_load_pct();
    int battery_pct = hu_scheduler_probe_battery_pct();
    bool on_ac = hu_scheduler_probe_on_ac_power();
    bool quiet = hu_scheduler_probe_quiet_hours(now_ms, s->persona);

    /* 2026-05-16 P3-5: when scoped, filter by contact_id at the SQL layer.
     * Equality match — never LIKE / prefix, to dodge the prefix-match class.
     * Legacy `hu_scheduler_tick` skips the filter (scoped=false) so daemon-
     * wide background drains keep working for autodream/lora/counterfactual. */
    const char *cid = target_contact_id ? target_contact_id : "";
    int cid_len = target_contact_id ? (int)target_contact_id_len : 0;

    /* Snapshot the eligible set first, then dispatch — keeping the SELECT
     * statement open across runner calls would block schema use by the
     * runners themselves. */
    static const char *const sel_sql_legacy =
        "SELECT id, kind, priority, budget_ms, interval_sec, "
        "requires_idle, requires_ac_power, latest_at, contact_id "
        "FROM scheduler_jobs "
        "WHERE status = 'pending' AND (earliest_at = 0 OR earliest_at <= ?) "
        "ORDER BY priority DESC, earliest_at ASC, id ASC LIMIT ?";
    static const char *const sel_sql_scoped =
        "SELECT id, kind, priority, budget_ms, interval_sec, "
        "requires_idle, requires_ac_power, latest_at, contact_id "
        "FROM scheduler_jobs "
        "WHERE status = 'pending' AND (earliest_at = 0 OR earliest_at <= ?) "
        "  AND (contact_id = '' OR contact_id = ?) "
        "ORDER BY priority DESC, earliest_at ASC, id ASC LIMIT ?";
    const char *sel_sql = scoped ? sel_sql_scoped : sel_sql_legacy;
    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(db, sel_sql, -1, &sel, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(sel, 1, now_ms);
    int next_bind = 2;
    if (scoped) {
        sqlite3_bind_text(sel, next_bind++, cid, cid_len, SQLITE_STATIC);
    }
    sqlite3_bind_int(sel, next_bind, HU_SCHED_MAX_JOBS_PER_TICK);

    dispatch_row_t rows[HU_SCHED_MAX_JOBS_PER_TICK];
    size_t nrows = 0;
    while (sqlite3_step(sel) == SQLITE_ROW && nrows < HU_SCHED_MAX_JOBS_PER_TICK) {
        dispatch_row_t *r = &rows[nrows++];
        r->id = sqlite3_column_int64(sel, 0);
        r->kind = sqlite3_column_int(sel, 1);
        r->priority = sqlite3_column_int(sel, 2);
        r->budget_ms = sqlite3_column_int(sel, 3);
        r->interval_sec = sqlite3_column_int(sel, 4);
        r->requires_idle = sqlite3_column_int(sel, 5);
        r->requires_ac_power = sqlite3_column_int(sel, 6);
        r->latest_at = sqlite3_column_int64(sel, 7);
        /* Pull contact_id for the spec we pass to the runner. */
        const unsigned char *row_cid = sqlite3_column_text(sel, 8);
        const char *row_cid_s = row_cid ? (const char *)row_cid : "";
        size_t row_cid_n = strnlen(row_cid_s, sizeof(r->contact_id) - 1);
        memcpy(r->contact_id, row_cid_s, row_cid_n);
        r->contact_id[row_cid_n] = '\0';
        r->contact_id_len = row_cid_n;
    }
    sqlite3_finalize(sel);

    int64_t tick_start = scheduler_now_ms();
    int64_t total_elapsed = 0;
    (void)battery_pct;

    if (nrows > 0)
        hu_log_info("scheduler", NULL,
                    "tick: %zu pending job(s) eligible for dispatch (load=%d%% ac=%d quiet=%d)",
                    nrows, load_pct, on_ac ? 1 : 0, quiet ? 1 : 0);

    for (size_t i = 0; i < nrows; i++) {
        dispatch_row_t *r = &rows[i];
        if (total_elapsed > HU_SCHED_TOTAL_BUDGET_MS)
            break; /* respect total tick budget across all runners */

        if (r->kind < 0 || r->kind >= HU_JOB_KIND_MAX) {
            update_status(db, r->id, "failed", "invalid kind", (int64_t)time(NULL) * 1000, 0,
                          now_ms);
            continue;
        }

        const char *reason = NULL;
        if (!job_eligible(r, load_pct, battery_pct, on_ac, quiet, &reason)) {
            hu_log_info("scheduler", NULL, "job id=%lld kind=%d deferred: %s (load=%d%%)",
                        (long long)r->id, r->kind, reason ? reason : "?", load_pct);
            /* Don't change status: leave job pending so a later tick
             * (under different system conditions) can pick it up.  We
             * record the reason as a transient note via last_error so
             * status snapshots have something to surface. */
            sqlite3_stmt *up = NULL;
            static const char *const usql = "UPDATE scheduler_jobs SET last_error = ? WHERE id = ?";
            if (sqlite3_prepare_v2(db, usql, -1, &up, NULL) == SQLITE_OK) {
                sqlite3_bind_text(up, 1, reason, -1, SQLITE_STATIC);
                sqlite3_bind_int64(up, 2, r->id);
                sqlite3_step(up);
                sqlite3_finalize(up);
            }
            continue;
        }

        int64_t job_budget = r->budget_ms > 0 ? r->budget_ms : HU_SCHED_DEFAULT_JOB_BUDGET_MS;
        int64_t remaining_total = HU_SCHED_TOTAL_BUDGET_MS - total_elapsed;
        if (job_budget > remaining_total)
            job_budget = remaining_total;

        runner_slot_t slot = s->runners[r->kind];
        hu_job_spec_t spec = {0};
        spec.kind = (hu_job_kind_t)r->kind;
        spec.priority = r->priority;
        spec.budget_ms = (int)job_budget;
        spec.interval_sec = r->interval_sec;
        spec.requires_idle = r->requires_idle != 0;
        spec.requires_ac_power = r->requires_ac_power != 0;
        spec.latest_at = r->latest_at;
        /* 2026-05-16 P3-5: propagate the stored contact_id to the runner so
         * downstream consumers (persona evolver, autodream phases) see the
         * scope the job was enqueued for, not the active turn's contact. */
        if (r->contact_id_len > 0) {
            spec.contact_id = r->contact_id;
            spec.contact_id_len = r->contact_id_len;
        }

        hu_log_info("scheduler", NULL, "dispatch job id=%lld kind=%d budget_ms=%lld",
                    (long long)r->id, r->kind, (long long)job_budget);
        set_running(db, r->id);
        int64_t job_start = scheduler_now_ms();
        hu_error_t rc = slot.fn ? slot.fn(s->m, &spec, job_budget, slot.user_data)
                                : default_noop_runner(s->m, &spec, job_budget, NULL);
        int64_t job_elapsed = scheduler_now_ms() - job_start;
        total_elapsed = scheduler_now_ms() - tick_start;

        const char *status_str = "done";
        const char *err = NULL;
        if (rc != HU_OK) {
            status_str = "failed";
            err = hu_error_string(rc);
            hu_log_warn("scheduler", NULL, "job id=%lld kind=%d finished %s (%s) elapsed_ms=%lld",
                        (long long)r->id, r->kind, status_str, err ? err : "?",
                        (long long)job_elapsed);
        } else if (job_elapsed > job_budget + 5 /* small grace */) {
            /* Runner overran its declared budget. We can't preempt
             * already-returned C code, but we can record the violation
             * so observability surfaces it. */
            status_str = "failed";
            err = "budget_exceeded";
            hu_log_warn("scheduler", NULL, "job id=%lld kind=%d budget_exceeded elapsed_ms=%lld",
                        (long long)r->id, r->kind, (long long)job_elapsed);
        } else {
            hu_log_info("scheduler", NULL, "job id=%lld kind=%d completed ok elapsed_ms=%lld",
                        (long long)r->id, r->kind, (long long)job_elapsed);
        }
        update_status(db, r->id, status_str, err, (int64_t)time(NULL) * 1000, r->interval_sec,
                      now_ms);
    }

    return HU_OK;
}

hu_error_t hu_scheduler_status(hu_scheduler_t *s, hu_scheduler_status_t *out) {
    if (!s || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->system_load_pct = hu_scheduler_probe_load_pct();
    out->battery_pct = hu_scheduler_probe_battery_pct();
    out->on_ac_power = hu_scheduler_probe_on_ac_power();
    out->quiet_hours_active =
        hu_scheduler_probe_quiet_hours((int64_t)time(NULL) * 1000, s->persona);

    struct sqlite3 *db = get_db(s->m);
    if (!db)
        return HU_OK;

    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM scheduler_jobs WHERE status = 'pending'", -1,
                           &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW)
            out->jobs_pending = (size_t)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }

    /* "Today" = last 24 hours, simplest definition. */
    int64_t cutoff = (int64_t)time(NULL) * 1000 - 24LL * 3600 * 1000;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM scheduler_jobs "
                           "WHERE status = 'done' AND last_run_at > ?",
                           -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(st, 1, cutoff);
        if (sqlite3_step(st) == SQLITE_ROW)
            out->jobs_completed_today = (size_t)sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    return HU_OK;
}

#else /* !HU_ENABLE_SQLITE */

hu_error_t hu_scheduler_enqueue(hu_scheduler_t *s, const hu_job_spec_t *job) {
    (void)s;
    (void)job;
    return HU_ERR_NOT_SUPPORTED;
}
hu_error_t hu_scheduler_tick(hu_scheduler_t *s, int64_t now_ms) {
    (void)s;
    (void)now_ms;
    return HU_ERR_NOT_SUPPORTED;
}
hu_error_t hu_scheduler_tick_for(hu_scheduler_t *s, int64_t now_ms, const char *target_contact_id,
                                 size_t target_contact_id_len) {
    (void)s;
    (void)now_ms;
    (void)target_contact_id;
    (void)target_contact_id_len;
    return HU_ERR_NOT_SUPPORTED;
}
hu_error_t hu_scheduler_status(hu_scheduler_t *s, hu_scheduler_status_t *out) {
    (void)s;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->battery_pct = -1;
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */
