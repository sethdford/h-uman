/* Exercises src/daemon/daemon_job_queue.c: HU_JOB_QUEUE=off does nothing at
 * start (no jobs table), shadow/live create the table and recover a row the
 * previous process left in `sending` to `unknown`, and the counters report
 * what the start did. */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE
#include "human/daemon/job_queue.h"
#include "human/memory.h"
#include "human/memory/job_queue_repo.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

static int jq_table_exists(sqlite3 *db) {
    sqlite3_stmt *st = NULL;
    int found = -1;
    if (sqlite3_prepare_v2(db,
                           "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND "
                           "name='jobs';",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        found = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return found;
}

static char *jq_saved_env(void) {
    const char *v = getenv("HU_JOB_QUEUE");
    return v ? strdup(v) : NULL;
}

static void jq_restore_env(char *saved) {
    if (saved) {
        setenv("HU_JOB_QUEUE", saved, 1);
        free(saved);
    } else {
        unsetenv("HU_JOB_QUEUE");
    }
}

/* Leave one row in `sending`, as a process killed mid-send would. */
static int64_t jq_seed_in_flight(sqlite3 *db) {
    if (hu_job_queue_repo_ensure_schema(db) != HU_OK)
        return -1;
    hu_job_spec_t s;
    memset(&s, 0, sizeof(s));
    s.kind = HU_JOB_KIND_SCHED_SEND;
    s.payload = "x";
    s.payload_len = 1;
    s.due_at = 1000;
    s.idempotency_key = "sched:seed";
    int64_t id = 0;
    hu_job_t j;
    size_t n = 0;
    if (hu_job_queue_repo_enqueue(db, &s, 900, &id, NULL) != HU_OK ||
        hu_job_queue_repo_claim_due(db, NULL, 1000, 120, &j, 1, &n) != HU_OK || n != 1 ||
        hu_job_queue_repo_mark_sending(db, id, j.lease_until, 1001) != HU_OK)
        return -1;
    return id;
}

static void daemon_job_queue_off_creates_no_table(void) {
    char *saved = jq_saved_env();
    unsetenv("HU_JOB_QUEUE");
    hu_daemon_job_queue_reset_for_test();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(jq_table_exists(db), 0);

    HU_ASSERT_EQ(hu_daemon_job_queue_mode(), HU_GATE_OFF);
    HU_ASSERT_EQ(hu_daemon_job_queue_start(db, NULL), HU_OK);
    HU_ASSERT_EQ(jq_table_exists(db), 0);
    /* An unparseable value fails closed to OFF too. */
    setenv("HU_JOB_QUEUE", "maybe", 1);
    HU_ASSERT_EQ(hu_daemon_job_queue_start(db, NULL), HU_OK);
    HU_ASSERT_EQ(jq_table_exists(db), 0);
    /* OFF needs no handle at all. */
    HU_ASSERT_EQ(hu_daemon_job_queue_start(NULL, NULL), HU_OK);

    hu_daemon_job_queue_metrics_t m;
    hu_daemon_job_queue_metrics(&m);
    HU_ASSERT_EQ(m.mode, HU_GATE_OFF);
    HU_ASSERT_FALSE(m.started);
    HU_ASSERT_EQ(m.start_failures, 0);
    mem.vtable->deinit(mem.ctx);
    jq_restore_env(saved);
}

static void daemon_job_queue_shadow_recovers_in_flight_row_to_unknown(void) {
    char *saved = jq_saved_env();
    hu_daemon_job_queue_reset_for_test();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    int64_t id = jq_seed_in_flight(db);
    HU_ASSERT_TRUE(id > 0);
    hu_job_queue_counts_t c;
    HU_ASSERT_EQ(hu_job_queue_repo_counts(db, &c), HU_OK);
    HU_ASSERT_EQ(c.sending, 1);
    HU_ASSERT_EQ(c.unknown, 0);

    setenv("HU_JOB_QUEUE", "shadow", 1);
    HU_ASSERT_EQ(hu_daemon_job_queue_mode(), HU_GATE_SHADOW);
    HU_ASSERT_EQ(hu_daemon_job_queue_start(db, NULL), HU_OK);

    HU_ASSERT_EQ(hu_job_queue_repo_counts(db, &c), HU_OK);
    HU_ASSERT_EQ(c.sending, 0);
    HU_ASSERT_EQ(c.unknown, 1);
    hu_daemon_job_queue_metrics_t m;
    hu_daemon_job_queue_metrics(&m);
    HU_ASSERT_EQ(m.mode, HU_GATE_SHADOW);
    HU_ASSERT_TRUE(m.started);
    HU_ASSERT_EQ(m.recovered_unknown, 1);
    HU_ASSERT_EQ(m.at_start.unknown, 1);
    HU_ASSERT_EQ(m.start_failures, 0);

    /* The recovered row is never handed out again. */
    hu_job_t j;
    size_t n = 9;
    HU_ASSERT_EQ(hu_job_queue_repo_claim_due(db, NULL, 99999, 120, &j, 1, &n), HU_OK);
    HU_ASSERT_EQ(n, 0);
    mem.vtable->deinit(mem.ctx);
    jq_restore_env(saved);
}

static void daemon_job_queue_live_creates_table_and_null_db_is_counted(void) {
    char *saved = jq_saved_env();
    hu_daemon_job_queue_reset_for_test();
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    setenv("HU_JOB_QUEUE", "live", 1);
    HU_ASSERT_EQ(jq_table_exists(db), 0);
    HU_ASSERT_EQ(hu_daemon_job_queue_start(db, NULL), HU_OK);
    HU_ASSERT_EQ(jq_table_exists(db), 1);
    hu_daemon_job_queue_metrics_t m;
    hu_daemon_job_queue_metrics(&m);
    HU_ASSERT_EQ(m.mode, HU_GATE_LIVE);
    HU_ASSERT_TRUE(m.started);
    HU_ASSERT_EQ(m.recovered_unknown, 0);

    HU_ASSERT_EQ(hu_daemon_job_queue_start(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    hu_daemon_job_queue_metrics(&m);
    HU_ASSERT_FALSE(m.started);
    HU_ASSERT_EQ(m.start_failures, 1);
    mem.vtable->deinit(mem.ctx);
    jq_restore_env(saved);
}

void run_daemon_job_queue_tests(void) {
    HU_TEST_SUITE("daemon job queue");
    HU_RUN_TEST(daemon_job_queue_off_creates_no_table);
    HU_RUN_TEST(daemon_job_queue_shadow_recovers_in_flight_row_to_unknown);
    HU_RUN_TEST(daemon_job_queue_live_creates_table_and_null_db_is_counted);
}
#else
void run_daemon_job_queue_tests(void) {}
#endif
