/* W14 — Sleep-Time Compute Scheduler tests.
 *
 * Covers the spec test plan and the additional scenarios called out in
 * the W14 brief.  Tests run against an in-memory SQLite DB (graph_open
 * with NULL path) so no on-disk artefacts.  All system-state probes
 * are driven by HU_TEST_* env vars — no test ever touches /proc,
 * IOKit, or GetSystemPowerStatus. */

#include "human/agent/scheduler.h"
#include "human/core/allocator.h"
#include "human/memory/graph.h"
#include "human/memory/memory.h"
#include "human/memory/neural_memory.h"
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static hu_allocator_t g_alloc;
static hu_allocator_t *A(void) {
    g_alloc = hu_system_allocator();
    return &g_alloc;
}

#ifdef HU_ENABLE_SQLITE

/* ── Shared test fixtures ─────────────────────────────────────────────── */

static void clear_w14_env(void) {
    unsetenv("HU_TEST_LOAD_PCT");
    unsetenv("HU_TEST_BATTERY_PCT");
    unsetenv("HU_TEST_ON_AC");
    unsetenv("HU_TEST_QUIET_HOURS");
}

static void open_stack_(hu_graph_t **g, hu_memory_facade_t **m, hu_scheduler_t **s) {
    HU_ASSERT_EQ(hu_graph_open(A(), NULL, 0, g), HU_OK);
    HU_ASSERT_NOT_NULL(*g);
    HU_ASSERT_EQ(hu_memory_facade_open(A(), *g, m), HU_OK);
    HU_ASSERT_NOT_NULL(*m);
    HU_ASSERT_EQ(hu_scheduler_open(A(), *m, s), HU_OK);
    HU_ASSERT_NOT_NULL(*s);
}

static void close_stack_(hu_graph_t *g, hu_memory_facade_t *m, hu_scheduler_t *s) {
    hu_scheduler_close(s, A());
    hu_memory_facade_close(m, A());
    hu_graph_close(g, A());
}

static int count_jobs_with_status(struct sqlite3 *db, const char *status) {
    sqlite3_stmt *st = NULL;
    int n = -1;
    if (sqlite3_prepare_v2(db,
                            "SELECT COUNT(*) FROM scheduler_jobs WHERE status = ?",
                            -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, status, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

/* Pack (kind, priority) into a single int for assertion-friendly logs. */
static int dispatch_log[256];
static size_t dispatch_log_len;

static hu_error_t recording_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec,
                                    int64_t budget_ms, void *user_data) {
    (void)m;
    (void)budget_ms;
    (void)user_data;
    if (dispatch_log_len < sizeof(dispatch_log) / sizeof(dispatch_log[0]))
        dispatch_log[dispatch_log_len++] = (int)spec->kind * 100 + spec->priority;
    return HU_OK;
}

static int g_increment_counter;
static hu_error_t increment_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec,
                                    int64_t budget_ms, void *user_data) {
    (void)m;
    (void)spec;
    (void)budget_ms;
    int *p = (int *)user_data;
    if (p)
        (*p)++;
    return HU_OK;
}

static hu_error_t error_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec,
                                int64_t budget_ms, void *user_data) {
    (void)m;
    (void)spec;
    (void)budget_ms;
    (void)user_data;
    return HU_ERR_INTERNAL;
}

static hu_error_t slow_runner(hu_memory_facade_t *m, const hu_job_spec_t *spec,
                               int64_t budget_ms, void *user_data) {
    (void)m;
    (void)spec;
    (void)budget_ms;
    (void)user_data;
    /* Sleep ~250 ms — well past the 50 ms budget the test enqueues. */
    struct timespec ts = {0, 250 * 1000 * 1000};
    nanosleep(&ts, NULL);
    return HU_OK;
}

/* ── Tests ────────────────────────────────────────────────────────────── */

static void test_w14_open_close_round_trip(void) {
    clear_w14_env();
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_scheduler_t *s = NULL;
    open_stack_(&g, &m, &s);

    /* Default no-op runner runs cleanly. */
    hu_job_spec_t job = {0};
    job.kind = HU_JOB_BELIEF_REVERIFICATION;
    job.budget_ms = 100;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &job), HU_OK);
    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690000000LL), HU_OK);

    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    HU_ASSERT_EQ(count_jobs_with_status(db, "done"), 1);
    HU_ASSERT_EQ(count_jobs_with_status(db, "pending"), 0);

    close_stack_(g, m, s);
}

static void test_w14_enqueue_invalid_args_rejected(void) {
    clear_w14_env();
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_scheduler_t *s = NULL;
    open_stack_(&g, &m, &s);

    HU_ASSERT_EQ(hu_scheduler_enqueue(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, NULL), HU_ERR_INVALID_ARGUMENT);

    hu_job_spec_t bad = {0};
    bad.kind = (hu_job_kind_t)999;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &bad), HU_ERR_INVALID_ARGUMENT);

    bad.kind = HU_JOB_AUTODREAM_DECAY;
    bad.budget_ms = -1;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &bad), HU_ERR_INVALID_ARGUMENT);

    bad.budget_ms = 100;
    bad.earliest_at = 200;
    bad.latest_at = 100; /* before earliest_at */
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &bad), HU_ERR_INVALID_ARGUMENT);

    /* Sanity: a well-formed spec is accepted. */
    hu_job_spec_t ok = {0};
    ok.kind = HU_JOB_AUTODREAM_DECAY;
    ok.budget_ms = 100;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &ok), HU_OK);

    close_stack_(g, m, s);
}

static void test_w14_scheduler_respects_priority(void) {
    clear_w14_env();
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_scheduler_t *s = NULL;
    open_stack_(&g, &m, &s);
    HU_ASSERT_EQ(hu_scheduler_register_runner(s, HU_JOB_LORA_TRAINING,
                                              recording_runner, NULL),
                 HU_OK);

    dispatch_log_len = 0;
    hu_job_spec_t job = {0};
    job.kind = HU_JOB_LORA_TRAINING;
    job.budget_ms = 50;

    job.priority = 0;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &job), HU_OK);
    job.priority = 1;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &job), HU_OK);
    job.priority = 0;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &job), HU_OK);

    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690000000LL), HU_OK);
    HU_ASSERT_EQ((int)dispatch_log_len, 3);
    /* Priority-1 job must run first; the two priority-0 jobs follow. */
    HU_ASSERT_EQ(dispatch_log[0] % 100, 1);
    HU_ASSERT_EQ(dispatch_log[1] % 100, 0);
    HU_ASSERT_EQ(dispatch_log[2] % 100, 0);

    close_stack_(g, m, s);
}

static void test_w14_scheduler_skips_jobs_on_battery_when_required(void) {
    clear_w14_env();
    setenv("HU_TEST_ON_AC", "0", 1);
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_scheduler_t *s = NULL;
    open_stack_(&g, &m, &s);
    int counter = 0;
    HU_ASSERT_EQ(hu_scheduler_register_runner(s, HU_JOB_KV_CACHE_WARMING,
                                              increment_runner, &counter),
                 HU_OK);

    hu_job_spec_t job = {0};
    job.kind = HU_JOB_KV_CACHE_WARMING;
    job.budget_ms = 50;
    job.requires_ac_power = true;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &job), HU_OK);

    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690000000LL), HU_OK);
    HU_ASSERT_EQ(counter, 0);
    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    HU_ASSERT_EQ(count_jobs_with_status(db, "pending"), 1);

    /* Plug in: AC available → job runs on next tick. */
    setenv("HU_TEST_ON_AC", "1", 1);
    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690001000LL), HU_OK);
    HU_ASSERT_EQ(counter, 1);
    HU_ASSERT_EQ(count_jobs_with_status(db, "done"), 1);

    clear_w14_env();
    close_stack_(g, m, s);
}

static void test_w14_scheduler_respects_quiet_hours(void) {
    clear_w14_env();
    setenv("HU_TEST_QUIET_HOURS", "1", 1);
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_scheduler_t *s = NULL;
    open_stack_(&g, &m, &s);
    HU_ASSERT_EQ(hu_scheduler_register_runner(s, HU_JOB_AUTODREAM_DECAY,
                                              recording_runner, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_scheduler_register_runner(s, HU_JOB_LORA_TRAINING,
                                              recording_runner, NULL),
                 HU_OK);

    dispatch_log_len = 0;

    hu_job_spec_t normal = {0};
    normal.kind = HU_JOB_AUTODREAM_DECAY;
    normal.budget_ms = 50;
    normal.priority = 0;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &normal), HU_OK);

    hu_job_spec_t emergency = {0};
    emergency.kind = HU_JOB_LORA_TRAINING;
    emergency.budget_ms = 50;
    emergency.priority = 2; /* HU_SCHED_EMERGENCY_PRIORITY */
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &emergency), HU_OK);

    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690000000LL), HU_OK);
    /* Only the emergency job dispatches; the normal one stays pending. */
    HU_ASSERT_EQ((int)dispatch_log_len, 1);
    HU_ASSERT_EQ(dispatch_log[0] / 100, (int)HU_JOB_LORA_TRAINING);
    HU_ASSERT_EQ(dispatch_log[0] % 100, 2);

    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    HU_ASSERT_EQ(count_jobs_with_status(db, "pending"), 1);

    clear_w14_env();
    close_stack_(g, m, s);
}

static void test_w14_scheduler_budget_enforced_per_job(void) {
    clear_w14_env();
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_scheduler_t *s = NULL;
    open_stack_(&g, &m, &s);
    HU_ASSERT_EQ(hu_scheduler_register_runner(s, HU_JOB_KV_CACHE_EVICTION,
                                              slow_runner, NULL),
                 HU_OK);

    hu_job_spec_t job = {0};
    job.kind = HU_JOB_KV_CACHE_EVICTION;
    job.budget_ms = 50;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &job), HU_OK);

    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690000000LL), HU_OK);
    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    /* Runner overran its budget → marked failed. */
    HU_ASSERT_EQ(count_jobs_with_status(db, "failed"), 1);
    HU_ASSERT_EQ(count_jobs_with_status(db, "done"), 0);

    close_stack_(g, m, s);
}

static void test_w14_autodream_runner_registered_by_default(void) {
    clear_w14_env();
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_scheduler_t *s = NULL;
    open_stack_(&g, &m, &s);

    /* No custom registration — rely on the autodream wrapper installed
     * at hu_scheduler_open.  The wrapper calls into hu_autodream_run
     * with selective phase enables; on an empty in-memory graph this
     * succeeds without crashing. */
    hu_job_spec_t q = {0};
    q.kind = HU_JOB_AUTODREAM_QUARANTINE;
    q.budget_ms = 200;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &q), HU_OK);

    hu_job_spec_t c = {0};
    c.kind = HU_JOB_AUTODREAM_COMMUNITY;
    c.budget_ms = 200;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &c), HU_OK);

    hu_job_spec_t d = {0};
    d.kind = HU_JOB_AUTODREAM_DECAY;
    d.budget_ms = 200;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &d), HU_OK);

    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690000000LL), HU_OK);
    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    HU_ASSERT_EQ(count_jobs_with_status(db, "done"), 3);

    close_stack_(g, m, s);
}

static void test_w14_register_custom_runner_dispatches_correctly(void) {
    clear_w14_env();
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_scheduler_t *s = NULL;
    open_stack_(&g, &m, &s);

    int counter = 0;
    HU_ASSERT_EQ(hu_scheduler_register_runner(s, HU_JOB_BELIEF_REVERIFICATION,
                                              increment_runner, &counter),
                 HU_OK);

    hu_job_spec_t job = {0};
    job.kind = HU_JOB_BELIEF_REVERIFICATION;
    job.budget_ms = 50;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &job), HU_OK);

    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690000000LL), HU_OK);
    HU_ASSERT_EQ(counter, 1);

    /* Re-registering with NULL resets to no-op and won't increment. */
    HU_ASSERT_EQ(hu_scheduler_register_runner(s, HU_JOB_BELIEF_REVERIFICATION,
                                              NULL, NULL),
                 HU_OK);
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &job), HU_OK);
    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690001000LL), HU_OK);
    HU_ASSERT_EQ(counter, 1);

    close_stack_(g, m, s);
}

static void test_w14_status_returns_pending_count(void) {
    clear_w14_env();
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_scheduler_t *s = NULL;
    open_stack_(&g, &m, &s);

    /* Far-future earliest_at keeps each job pending. */
    int64_t future = (int64_t)time(NULL) * 1000 + 10LL * 365 * 24 * 3600 * 1000;
    hu_job_spec_t job = {0};
    job.kind = HU_JOB_KV_CACHE_EVICTION;
    job.budget_ms = 100;
    job.earliest_at = future;
    for (int i = 0; i < 5; i++)
        HU_ASSERT_EQ(hu_scheduler_enqueue(s, &job), HU_OK);

    hu_scheduler_status_t st;
    HU_ASSERT_EQ(hu_scheduler_status(s, &st), HU_OK);
    HU_ASSERT_EQ((int)st.jobs_pending, 5);
    HU_ASSERT_EQ((int)st.jobs_completed_today, 0);

    close_stack_(g, m, s);
}

static void test_w14_adversarial_job_flood_respects_total_budget(void) {
    clear_w14_env();
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_scheduler_t *s = NULL;
    open_stack_(&g, &m, &s);

    g_increment_counter = 0;
    HU_ASSERT_EQ(hu_scheduler_register_runner(s, HU_JOB_KV_CACHE_WARMING,
                                              increment_runner, &g_increment_counter),
                 HU_OK);

    hu_job_spec_t job = {0};
    job.kind = HU_JOB_KV_CACHE_WARMING;
    job.budget_ms = 50;
    for (int i = 0; i < 1000; i++)
        HU_ASSERT_EQ(hu_scheduler_enqueue(s, &job), HU_OK);

    /* Single tick must be bounded — the per-tick cap takes precedence
     * over the queue depth. */
    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690000000LL), HU_OK);
    HU_ASSERT(g_increment_counter <= 32);
    HU_ASSERT(g_increment_counter > 0);

    /* Subsequent ticks drain more — but each tick remains capped. */
    int prev = g_increment_counter;
    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690001000LL), HU_OK);
    HU_ASSERT(g_increment_counter > prev);
    HU_ASSERT(g_increment_counter - prev <= 32);

    close_stack_(g, m, s);
}

static void test_w14_adversarial_runner_returns_error_does_not_crash_scheduler(void) {
    clear_w14_env();
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_scheduler_t *s = NULL;
    open_stack_(&g, &m, &s);

    HU_ASSERT_EQ(hu_scheduler_register_runner(s, HU_JOB_LORA_TRAINING,
                                              error_runner, NULL),
                 HU_OK);
    int counter = 0;
    HU_ASSERT_EQ(hu_scheduler_register_runner(s, HU_JOB_KV_CACHE_WARMING,
                                              increment_runner, &counter),
                 HU_OK);

    hu_job_spec_t bad = {0};
    bad.kind = HU_JOB_LORA_TRAINING;
    bad.budget_ms = 50;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &bad), HU_OK);

    hu_job_spec_t good = {0};
    good.kind = HU_JOB_KV_CACHE_WARMING;
    good.budget_ms = 50;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &good), HU_OK);

    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690000000LL), HU_OK);
    HU_ASSERT_EQ(counter, 1);

    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    HU_ASSERT_EQ(count_jobs_with_status(db, "failed"), 1);
    HU_ASSERT_EQ(count_jobs_with_status(db, "done"), 1);

    close_stack_(g, m, s);
}

static void test_w14_counterfactual_rehearsal_caps_at_five_per_tick(void) {
    clear_w14_env();
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_scheduler_t *s = NULL;
    open_stack_(&g, &m, &s);

    /* Set up: contact 'cf', 1 trace recorded at t=1000, 8 relations
     * upserted "now" so each relation's last_seen > trace.recorded_at. */
    int64_t self_id = 0;
    HU_ASSERT_EQ(hu_graph_upsert_entity(g, "cf", 2, "self", 4, HU_ENTITY_PERSON,
                                        NULL, &self_id),
                 HU_OK);
    for (int i = 0; i < 8; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "fact%d", i);
        int64_t target_id = 0;
        HU_ASSERT_EQ(hu_graph_upsert_entity(g, "cf", 2, nm, strlen(nm),
                                            HU_ENTITY_TOPIC, NULL, &target_id),
                     HU_OK);
        char ctx[64];
        snprintf(ctx, sizeof(ctx), "context %d", i);
        HU_ASSERT_EQ(hu_graph_upsert_relation(g, "cf", 2, self_id, target_id,
                                              HU_REL_KNOWS, 1.0f, ctx,
                                              strlen(ctx)),
                     HU_OK);
    }

    hu_reasoning_trace_t trace = {0};
    snprintf(trace.goal_verb, sizeof(trace.goal_verb), "schedule");
    char cot[] = "thought process";
    trace.cot_text = cot;
    trace.cot_len = strlen(cot);
    char outcome[] = "ok";
    trace.outcome = outcome;
    trace.recorded_at = 1000; /* far before relation upserts */
    trace.belief.mean = 0.8f;
    int64_t trace_id = 0;
    HU_ASSERT_EQ(hu_reasoning_trace_record(m, "cf", 2, &trace, &trace_id), HU_OK);

    HU_ASSERT_EQ(hu_scheduler_register_runner(s, HU_JOB_COUNTERFACTUAL_REHEARSAL,
                                              hu_counterfactual_rehearsal_runner,
                                              NULL),
                 HU_OK);
    hu_job_spec_t job = {0};
    job.kind = HU_JOB_COUNTERFACTUAL_REHEARSAL;
    job.budget_ms = 500;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &job), HU_OK);
    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690000000LL), HU_OK);

    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    sqlite3_stmt *st = NULL;
    int n = -1;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT COUNT(*) FROM counterfactual_replays",
                                    -1, &st, NULL),
                 SQLITE_OK);
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    HU_ASSERT_EQ(n, 5);

    /* Re-running the runner is idempotent for the already-rehearsed
     * pairs (NOT EXISTS guard); the remaining 3 pairs land on the
     * second tick. */
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &job), HU_OK);
    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690001000LL), HU_OK);
    n = -1;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db,
                                    "SELECT COUNT(*) FROM counterfactual_replays",
                                    -1, &st, NULL),
                 SQLITE_OK);
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    HU_ASSERT_EQ(n, 8);

    close_stack_(g, m, s);
}

static void test_w14_expired_jobs_marked_after_latest_at(void) {
    clear_w14_env();
    hu_graph_t *g = NULL;
    hu_memory_facade_t *m = NULL;
    hu_scheduler_t *s = NULL;
    open_stack_(&g, &m, &s);

    int counter = 0;
    HU_ASSERT_EQ(hu_scheduler_register_runner(s, HU_JOB_LORA_TRAINING,
                                              increment_runner, &counter),
                 HU_OK);

    hu_job_spec_t job = {0};
    job.kind = HU_JOB_LORA_TRAINING;
    job.budget_ms = 50;
    job.earliest_at = 1000;
    job.latest_at = 5000;
    HU_ASSERT_EQ(hu_scheduler_enqueue(s, &job), HU_OK);

    /* now_ms well past latest_at → job marked expired without running. */
    HU_ASSERT_EQ(hu_scheduler_tick(s, 1735690000000LL), HU_OK);
    HU_ASSERT_EQ(counter, 0);
    struct sqlite3 *db = hu_graph_sqlite_connection(g);
    HU_ASSERT_EQ(count_jobs_with_status(db, "expired"), 1);
    HU_ASSERT_EQ(count_jobs_with_status(db, "pending"), 0);

    close_stack_(g, m, s);
}

#endif /* HU_ENABLE_SQLITE */

/* ── Probe tests (no SQLite needed) ───────────────────────────────────── */

static void test_w14_probe_load_pct_honors_test_override(void) {
    clear_w14_env();
    setenv("HU_TEST_LOAD_PCT", "73", 1);
    HU_ASSERT_EQ(hu_scheduler_probe_load_pct(), 73);
    setenv("HU_TEST_LOAD_PCT", "0", 1);
    HU_ASSERT_EQ(hu_scheduler_probe_load_pct(), 0);
    clear_w14_env();
}

static void test_w14_probe_battery_pct_honors_test_override(void) {
    clear_w14_env();
    setenv("HU_TEST_BATTERY_PCT", "42", 1);
    HU_ASSERT_EQ(hu_scheduler_probe_battery_pct(), 42);
    clear_w14_env();
}

static void test_w14_probe_on_ac_honors_test_override(void) {
    clear_w14_env();
    setenv("HU_TEST_ON_AC", "0", 1);
    HU_ASSERT_EQ(hu_scheduler_probe_on_ac_power(), false);
    setenv("HU_TEST_ON_AC", "1", 1);
    HU_ASSERT_EQ(hu_scheduler_probe_on_ac_power(), true);
    clear_w14_env();
}

static void test_w14_probe_quiet_hours_honors_test_override(void) {
    clear_w14_env();
    setenv("HU_TEST_QUIET_HOURS", "1", 1);
    HU_ASSERT_EQ(hu_scheduler_probe_quiet_hours(0, NULL), true);
    setenv("HU_TEST_QUIET_HOURS", "0", 1);
    HU_ASSERT_EQ(hu_scheduler_probe_quiet_hours(0, NULL), false);
    clear_w14_env();
}

static void test_w14_probe_load_returns_in_range_or_unknown(void) {
    /* With test override unset the probe falls through to OS-level
     * paths.  All we can prove portably is that the value is either
     * -1 (unknown) or in [0,100]. */
    clear_w14_env();
    int v = hu_scheduler_probe_load_pct();
    HU_ASSERT_TRUE(v == -1 || (v >= 0 && v <= 100));
}

static void test_w14_probe_battery_returns_in_range_or_unknown(void) {
    clear_w14_env();
    int v = hu_scheduler_probe_battery_pct();
    HU_ASSERT_TRUE(v == -1 || (v >= 0 && v <= 100));
}

void run_w14_scheduler_tests(void) {
    HU_TEST_SUITE("W14 scheduler - sleep-time compute scheduler hu_scheduler_t + counterfactual rehearsal");

#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_w14_open_close_round_trip);
    HU_RUN_TEST(test_w14_enqueue_invalid_args_rejected);
    HU_RUN_TEST(test_w14_scheduler_respects_priority);
    HU_RUN_TEST(test_w14_scheduler_skips_jobs_on_battery_when_required);
    HU_RUN_TEST(test_w14_scheduler_respects_quiet_hours);
    HU_RUN_TEST(test_w14_scheduler_budget_enforced_per_job);
    HU_RUN_TEST(test_w14_autodream_runner_registered_by_default);
    HU_RUN_TEST(test_w14_register_custom_runner_dispatches_correctly);
    HU_RUN_TEST(test_w14_status_returns_pending_count);
    HU_RUN_TEST(test_w14_adversarial_job_flood_respects_total_budget);
    HU_RUN_TEST(test_w14_adversarial_runner_returns_error_does_not_crash_scheduler);
    HU_RUN_TEST(test_w14_counterfactual_rehearsal_caps_at_five_per_tick);
    HU_RUN_TEST(test_w14_expired_jobs_marked_after_latest_at);
#endif

    /* Probe tests run without SQLite — pure env/OS-state checks. */
    HU_RUN_TEST(test_w14_probe_load_pct_honors_test_override);
    HU_RUN_TEST(test_w14_probe_battery_pct_honors_test_override);
    HU_RUN_TEST(test_w14_probe_on_ac_honors_test_override);
    HU_RUN_TEST(test_w14_probe_quiet_hours_honors_test_override);
    HU_RUN_TEST(test_w14_probe_load_returns_in_range_or_unknown);
    HU_RUN_TEST(test_w14_probe_battery_returns_in_range_or_unknown);
}
