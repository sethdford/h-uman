/* tests/test_doctor_reflection_loop.c — T12 of
 * docs/plans/2026-05-26-reflection-loop. Pins the doctor check's
 * verdict truth table:
 *
 *   AC-T12.1  NULL ctx → NA "no config"
 *   AC-T12.2  cfg with enabled=false → NA "disabled in config"
 *   AC-T12.3  cfg enabled + db=NULL → NA "no db handle"
 *   AC-T12.4  cfg enabled + db with 0 runs → NA "cold start"
 *   AC-T12.5  cfg enabled + db with recent ok run → PASS "healthy"
 *   AC-T12.6  cfg enabled + db with 5 consecutive non-ok → FAIL "broken"
 *   AC-T12.7  cfg enabled + db with only stale ok runs → NA "stale"
 *
 * detail_json always present when we got past the early NAs. */

#include "human/config.h"
#include "human/config_types.h"
#include "human/doctor/check_reflection_loop.h"
#include "human/reflection.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

/* Insert a run row with a specific status + completed_at timestamp. */
static void insert_run(sqlite3 *db, const char *run_id, const char *status, uint64_t started_at_ms,
                       uint64_t completed_at_ms) {
    sqlite3_stmt *st = NULL;
    sqlite3_prepare_v2(db,
                       "INSERT INTO reflection_runs (run_id, provider, started_at_ms, "
                       "completed_at_ms, input_turns, status) VALUES (?, 'mock', ?, ?, 5, ?)",
                       -1, &st, NULL);
    sqlite3_bind_text(st, 1, run_id, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)started_at_ms);
    sqlite3_bind_int64(st, 3, (sqlite3_int64)completed_at_ms);
    sqlite3_bind_text(st, 4, status, -1, SQLITE_STATIC);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

static hu_reflection_loop_config_t enabled_cfg(void) {
    hu_reflection_loop_config_t c = {0};
    c.enabled = true;
    c.min_interval_hours = 12;
    c.idle_threshold_hours = 2;
    c.daily_floor_hours = 24;
    snprintf(c.provider, sizeof c.provider, "%s", "mock");
    return c;
}

/* hu_config_t is huge — use a stack-allocated stub that just carries
 * the reflection_loop field. The doctor check ONLY reads
 * cfg->reflection_loop, so a struct hu_config with just that field
 * populated suffices. (The doctor check uses the public hu_config_t
 * typedef but only dereferences ->reflection_loop.) */

static void seed_cfg(hu_config_t *cfg, hu_reflection_loop_config_t loop) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->reflection_loop = loop;
}

/* AC-T12.1 */
static void test_doctor_null_ctx_returns_na(void) {
    hu_doctor_check_result_t res =
        hu_doctor_check_reflection_loop.run(&hu_doctor_check_reflection_loop, NULL);
    HU_ASSERT_EQ((int)res.verdict, (int)HU_DOCTOR_NA);
    HU_ASSERT(res.reason != NULL);
    HU_ASSERT(strstr(res.reason, "no config") != NULL);
}

/* AC-T12.2 */
static void test_doctor_disabled_config_returns_na(void) {
    hu_config_t cfg;
    seed_cfg(&cfg, (hu_reflection_loop_config_t){.enabled = false});
    hu_doctor_check_reflection_loop_ctx_t rctx = {.cfg = &cfg, .db = NULL};
    hu_doctor_check_result_t res =
        hu_doctor_check_reflection_loop.run(&hu_doctor_check_reflection_loop, &rctx);
    HU_ASSERT_EQ((int)res.verdict, (int)HU_DOCTOR_NA);
    HU_ASSERT(strstr(res.reason, "disabled") != NULL);
}

/* AC-T12.3 */
static void test_doctor_enabled_no_db_returns_na(void) {
    hu_config_t cfg;
    seed_cfg(&cfg, enabled_cfg());
    hu_doctor_check_reflection_loop_ctx_t rctx = {.cfg = &cfg, .db = NULL};
    hu_doctor_check_result_t res =
        hu_doctor_check_reflection_loop.run(&hu_doctor_check_reflection_loop, &rctx);
    HU_ASSERT_EQ((int)res.verdict, (int)HU_DOCTOR_NA);
    HU_ASSERT(strstr(res.reason, "no db") != NULL);
}

/* AC-T12.4 */
static void test_doctor_enabled_zero_runs_returns_na_cold_start(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    hu_config_t cfg;
    seed_cfg(&cfg, enabled_cfg());
    hu_doctor_check_reflection_loop_ctx_t rctx = {.cfg = &cfg, .db = db};
    hu_doctor_check_result_t res =
        hu_doctor_check_reflection_loop.run(&hu_doctor_check_reflection_loop, &rctx);
    HU_ASSERT_EQ((int)res.verdict, (int)HU_DOCTOR_NA);
    HU_ASSERT(strstr(res.reason, "cold start") != NULL);
    HU_ASSERT(res.detail_json != NULL);
    HU_ASSERT(strstr(res.detail_json, "\"total\":0") != NULL);
    sqlite3_close(db);
}

/* AC-T12.5 */
static void test_doctor_recent_ok_run_returns_pass(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    /* OK run 2 hours ago. */
    insert_run(db, "run_ok", "ok", now_ms - 2 * 3600ULL * 1000ULL, now_ms - 2 * 3600ULL * 1000ULL);
    hu_config_t cfg;
    seed_cfg(&cfg, enabled_cfg());
    hu_doctor_check_reflection_loop_ctx_t rctx = {.cfg = &cfg, .db = db};
    hu_doctor_check_result_t res =
        hu_doctor_check_reflection_loop.run(&hu_doctor_check_reflection_loop, &rctx);
    HU_ASSERT_EQ((int)res.verdict, (int)HU_DOCTOR_PASS);
    HU_ASSERT(strstr(res.reason, "healthy") != NULL);
    HU_ASSERT(strstr(res.detail_json, "\"ok\":1") != NULL);
    sqlite3_close(db);
}

/* AC-T12.6 */
static void test_doctor_consecutive_non_ok_returns_fail(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    insert_run(db, "r1", "provider_error", now_ms - 5 * 3600ULL * 1000ULL,
               now_ms - 5 * 3600ULL * 1000ULL);
    insert_run(db, "r2", "schema_invalid", now_ms - 4 * 3600ULL * 1000ULL,
               now_ms - 4 * 3600ULL * 1000ULL);
    insert_run(db, "r3", "provider_error", now_ms - 3 * 3600ULL * 1000ULL,
               now_ms - 3 * 3600ULL * 1000ULL);
    insert_run(db, "r4", "provider_error", now_ms - 2 * 3600ULL * 1000ULL,
               now_ms - 2 * 3600ULL * 1000ULL);
    insert_run(db, "r5", "schema_invalid", now_ms - 1 * 3600ULL * 1000ULL,
               now_ms - 1 * 3600ULL * 1000ULL);
    hu_config_t cfg;
    seed_cfg(&cfg, enabled_cfg());
    hu_doctor_check_reflection_loop_ctx_t rctx = {.cfg = &cfg, .db = db};
    hu_doctor_check_result_t res =
        hu_doctor_check_reflection_loop.run(&hu_doctor_check_reflection_loop, &rctx);
    HU_ASSERT_EQ((int)res.verdict, (int)HU_DOCTOR_FAIL);
    HU_ASSERT(strstr(res.reason, "BROKEN") != NULL);
    HU_ASSERT(strstr(res.detail_json, "\"consecutive_non_ok\":5") != NULL);
    sqlite3_close(db);
}

/* AC-T12.7 */
static void test_doctor_only_stale_ok_returns_na(void) {
    sqlite3 *db = NULL;
    sqlite3_open(":memory:", &db);
    hu_reflection_storage_migrate(db);
    uint64_t now_ms = (uint64_t)time(NULL) * 1000ULL;
    /* OK run 30 days ago — well past the 7-day window. */
    uint64_t old_ms = now_ms - 30ULL * 86400ULL * 1000ULL;
    insert_run(db, "r_old", "ok", old_ms, old_ms);
    hu_config_t cfg;
    seed_cfg(&cfg, enabled_cfg());
    hu_doctor_check_reflection_loop_ctx_t rctx = {.cfg = &cfg, .db = db};
    hu_doctor_check_result_t res =
        hu_doctor_check_reflection_loop.run(&hu_doctor_check_reflection_loop, &rctx);
    HU_ASSERT_EQ((int)res.verdict, (int)HU_DOCTOR_NA);
    HU_ASSERT(strstr(res.reason, "stale") != NULL);
    sqlite3_close(db);
}

void run_doctor_reflection_loop_tests(void) {
    HU_TEST_SUITE("doctor_reflection_loop");
    HU_RUN_TEST(test_doctor_null_ctx_returns_na);
    HU_RUN_TEST(test_doctor_disabled_config_returns_na);
    HU_RUN_TEST(test_doctor_enabled_no_db_returns_na);
    HU_RUN_TEST(test_doctor_enabled_zero_runs_returns_na_cold_start);
    HU_RUN_TEST(test_doctor_recent_ok_run_returns_pass);
    HU_RUN_TEST(test_doctor_consecutive_non_ok_returns_fail);
    HU_RUN_TEST(test_doctor_only_stale_ok_returns_na);
}

#else /* !HU_ENABLE_SQLITE */

void run_doctor_reflection_loop_tests(void) {
    /* Stub when SQLite is disabled. */
}

#endif /* HU_ENABLE_SQLITE */
