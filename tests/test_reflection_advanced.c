#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/intelligence/feedback.h"
#include "human/intelligence/meta_learning.h"
#include "human/intelligence/reflection.h"
#include "human/memory.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <string.h>
#include <time.h>

static void test_reflection_weekly_inserts_self_evaluations_for_contacts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t now = 1700000000;
    int64_t week_start = now - (3 * 24 * 3600);

    hu_feedback_record(db, "response_style", 13, "contact_a", 9, "positive", 8, "warm reply", 9,
                       week_start);
    hu_feedback_record(db, "response_style", 13, "contact_a", 9, "negative", 8, "short", 5,
                       week_start + 3600);
    hu_feedback_record(db, "response_style", 13, "contact_b", 9, "positive", 8, "engaged", 7,
                       week_start + 7200);
    hu_feedback_record(db, "response_style", 13, "contact_b", 9, "positive", 8, "continued", 9,
                       week_start + 10800);

    hu_reflection_engine_t engine;
    hu_error_t err = hu_reflection_engine_create(&alloc, db, &engine);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_reflection_weekly(&engine, now);
    HU_ASSERT_EQ(err, HU_OK);

    sqlite3_stmt *sel = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT contact_id, week, metrics, recommendations FROM "
                                "self_evaluations ORDER BY contact_id",
                                -1, &sel, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    int row_count = 0;
    while (sqlite3_step(sel) == SQLITE_ROW) {
        row_count++;
        const char *cid = (const char *)sqlite3_column_text(sel, 0);
        int week = sqlite3_column_int(sel, 1);
        const char *metrics = (const char *)sqlite3_column_text(sel, 2);
        const char *rec = (const char *)sqlite3_column_text(sel, 3);
        HU_ASSERT_NOT_NULL(cid);
        HU_ASSERT_NOT_NULL(metrics);
        HU_ASSERT_NOT_NULL(rec);
        HU_ASSERT_TRUE(week == (int)(now / 604800));
        HU_ASSERT_TRUE(strcmp(rec, "maintain") == 0 || strcmp(rec, "improve") == 0);
    }
    sqlite3_finalize(sel);
    HU_ASSERT_EQ(row_count, 2);

    mem.vtable->deinit(mem.ctx);
}

static void test_reflection_extract_general_lessons_creates_lesson_from_two_contacts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t now = 1700000000;
    int64_t week = now / 604800;

    const char *ins =
        "INSERT INTO self_evaluations (contact_id, week, metrics, recommendations, created_at) "
        "VALUES (?1, ?2, ?3, ?4, ?5)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, ins, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);

    const char *metrics = "{\"positive\":2,\"negative\":1,\"neutral\":0,\"relationship_health\":0.5}";
    sqlite3_bind_text(stmt, 1, "contact_a", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, week);
    sqlite3_bind_text(stmt, 3, metrics, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, "maintain", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, now - (7 * 24 * 3600));
    rc = sqlite3_step(stmt);
    HU_ASSERT_EQ(rc, SQLITE_DONE);

    sqlite3_reset(stmt);
    sqlite3_bind_text(stmt, 1, "contact_b", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 2, week);
    sqlite3_bind_text(stmt, 3, metrics, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, "maintain", -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, now - (14 * 24 * 3600));
    rc = sqlite3_step(stmt);
    HU_ASSERT_EQ(rc, SQLITE_DONE);
    sqlite3_finalize(stmt);

    hu_reflection_engine_t engine;
    hu_error_t err = hu_reflection_engine_create(&alloc, db, &engine);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_reflection_extract_general_lessons(&engine, now);
    HU_ASSERT_EQ(err, HU_OK);

    sqlite3_stmt *sel = NULL;
    rc = sqlite3_prepare_v2(db, "SELECT lesson, source_count FROM general_lessons", -1, &sel, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(sel);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(sel, 0), "maintain");
    HU_ASSERT_EQ(sqlite3_column_int(sel, 1), 2);
    sqlite3_finalize(sel);

    mem.vtable->deinit(mem.ctx);
}

static void test_meta_learning_load_returns_defaults(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    hu_meta_params_t params = {0};
    hu_error_t err = hu_meta_learning_load(db, &params);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FLOAT_EQ(params.default_confidence_threshold, 0.7, 0.001);
    HU_ASSERT_EQ(params.refinement_frequency_weeks, 1);
    HU_ASSERT_EQ(params.discovery_min_feedback_count, 3);

    mem.vtable->deinit(mem.ctx);
}

static void test_meta_learning_update_persists_and_load_returns_values(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    hu_meta_params_t in = {
        .default_confidence_threshold = 0.85,
        .refinement_frequency_weeks = 2,
        .discovery_min_feedback_count = 5,
    };
    hu_error_t err = hu_meta_learning_update(db, &in);
    HU_ASSERT_EQ(err, HU_OK);

    hu_meta_params_t out = {0};
    err = hu_meta_learning_load(db, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FLOAT_EQ(out.default_confidence_threshold, 0.85, 0.001);
    HU_ASSERT_EQ(out.refinement_frequency_weeks, 2);
    HU_ASSERT_EQ(out.discovery_min_feedback_count, 5);

    mem.vtable->deinit(mem.ctx);
}

static void test_meta_learning_optimize_lowers_threshold_when_skills_succeed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t now = (int64_t)time(NULL);
    const char *ins_skill =
        "INSERT INTO skills (name, type, contact_id, trigger_conditions, strategy, success_rate, "
        "attempts, successes, version, origin, parent_skill_id, created_at, retired) "
        "VALUES ('test_skill', 'interpersonal', 'c1', NULL, 'Do X', 0.85, 10, 8, 1, 'manual', 0, "
        "?1, 0)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, ins_skill, -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, now);
    rc = sqlite3_step(stmt);
    HU_ASSERT_EQ(rc, SQLITE_DONE);
    sqlite3_finalize(stmt);

    hu_meta_params_t params = {0};
    hu_error_t err = hu_meta_learning_load(db, &params);
    HU_ASSERT_EQ(err, HU_OK);
    double before = params.default_confidence_threshold;

    err = hu_meta_learning_optimize(db, &params);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_TRUE(params.default_confidence_threshold < before);
    HU_ASSERT_FLOAT_EQ(params.default_confidence_threshold, before - 0.05, 0.001);

    mem.vtable->deinit(mem.ctx);
}

void run_reflection_advanced_tests(void) {
    HU_TEST_SUITE("reflection_advanced");
    HU_RUN_TEST(test_reflection_weekly_inserts_self_evaluations_for_contacts);
    HU_RUN_TEST(test_reflection_extract_general_lessons_creates_lesson_from_two_contacts);
    HU_RUN_TEST(test_meta_learning_load_returns_defaults);
    HU_RUN_TEST(test_meta_learning_update_persists_and_load_returns_values);
    HU_RUN_TEST(test_meta_learning_optimize_lowers_threshold_when_skills_succeed);
}

#else

void run_reflection_advanced_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
