#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/intelligence/reflection.h"
#include "test_framework.h"
#include <string.h>

#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include <sqlite3.h>
#endif

static void test_reflection_create_tables_sql_valid(void) {
    char buf[2048];
    size_t len = 0;
    hu_error_t err = hu_reflection_create_tables_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "feedback_signals") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "reflections") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "CREATE TABLE") != NULL);
}

static void test_feedback_insert_sql_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feedback_signal_t fb = {
        .contact_id = hu_strndup(&alloc, "user_a", 6),
        .contact_id_len = 6,
        .type = HU_FEEDBACK_POSITIVE,
        .context = hu_strndup(&alloc, "warm reply", 10),
        .context_len = 10,
        .our_action = hu_strndup(&alloc, "asked question", 14),
        .our_action_len = 14,
        .timestamp = 1000,
    };
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_feedback_insert_sql(&fb, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "user_a") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "INSERT INTO feedback_signals") != NULL);
    hu_feedback_signal_deinit(&alloc, &fb);
}

static void test_feedback_insert_sql_escapes_quotes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feedback_signal_t fb = {
        .contact_id = hu_strndup(&alloc, "O'Brien", 7),
        .contact_id_len = 7,
        .type = HU_FEEDBACK_NEUTRAL,
        .context = NULL,
        .context_len = 0,
        .our_action = NULL,
        .our_action_len = 0,
        .timestamp = 2000,
    };
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_feedback_insert_sql(&fb, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "''") != NULL);
    hu_feedback_signal_deinit(&alloc, &fb);
}

static void test_feedback_query_recent_sql_valid(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_feedback_query_recent_sql("contact_1", 9, 10, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "contact_1") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "ORDER BY timestamp DESC") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "LIMIT 10") != NULL);
}

static void test_reflection_insert_sql_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_reflection_entry_t entry = {
        .period = hu_strndup(&alloc, "daily", 5),
        .period_len = 5,
        .insights = hu_strndup(&alloc, "[\"insight1\"]", 12),
        .insights_len = 12,
        .improvements = hu_strndup(&alloc, "ask more", 8),
        .improvements_len = 8,
        .created_at = 3000,
    };
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_reflection_insert_sql(&entry, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "daily") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "INSERT INTO reflections") != NULL);
    hu_reflection_entry_deinit(&alloc, &entry);
}

static void test_reflection_query_latest_sql_valid(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_reflection_query_latest_sql("weekly", 6, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "weekly") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "ORDER BY created_at DESC") != NULL);
}

static void test_feedback_classify_left_on_read_negative(void) {
    hu_feedback_signal_type_t t =
        hu_reflection_feedback_classify(60, 100, true, true);
    HU_ASSERT_EQ(t, HU_FEEDBACK_NEGATIVE);
}

static void test_feedback_classify_short_response_negative(void) {
    hu_feedback_signal_type_t t = hu_reflection_feedback_classify(10, 3, false, false);
    HU_ASSERT_EQ(t, HU_FEEDBACK_NEGATIVE);
}

static void test_feedback_classify_contains_question_positive(void) {
    hu_feedback_signal_type_t t = hu_reflection_feedback_classify(30, 50, true, false);
    HU_ASSERT_EQ(t, HU_FEEDBACK_POSITIVE);
}

static void test_feedback_classify_otherwise_neutral(void) {
    hu_feedback_signal_type_t t = hu_reflection_feedback_classify(20, 20, false, false);
    HU_ASSERT_EQ(t, HU_FEEDBACK_NEUTRAL);
}

static void test_skill_proficiency_score_formula(void) {
    double s = hu_skill_proficiency_score(8, 10, 15);
    HU_ASSERT_FLOAT_EQ(s, 0.8, 0.001);
    double s2 = hu_skill_proficiency_score(5, 10, 5);
    HU_ASSERT_FLOAT_EQ(s2, 0.25, 0.001);
}

static void test_cross_contact_learning_weight_formula(void) {
    double w = hu_cross_contact_learning_weight(0.8, 0.5);
    HU_ASSERT_FLOAT_EQ(w, 0.2, 0.001);
}

static void test_reflection_build_prompt_with_feedback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_reflection_entry_t entry = {
        .period = hu_strndup(&alloc, "daily", 5),
        .period_len = 5,
        .insights = hu_strndup(&alloc, "ask more questions", 18),
        .insights_len = 18,
        .improvements = NULL,
        .improvements_len = 0,
        .created_at = 1000,
    };
    hu_feedback_signal_t feedback[2] = {
        {.type = HU_FEEDBACK_POSITIVE},
        {.type = HU_FEEDBACK_NEGATIVE},
    };
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_reflection_build_prompt(&alloc, &entry, feedback, 2, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "[RECENT LEARNING]") != NULL);
    HU_ASSERT_TRUE(strstr(out, "ask more questions") != NULL);
    HU_ASSERT_TRUE(strstr(out, "1 positive") != NULL);
    HU_ASSERT_TRUE(strstr(out, "1 negative") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
    hu_reflection_entry_deinit(&alloc, &entry);
}

static void test_reflection_build_prompt_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_reflection_build_prompt(&alloc, NULL, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "(none yet)") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_feedback_type_str_all_types(void) {
    HU_ASSERT_STR_EQ(hu_feedback_type_str(HU_FEEDBACK_POSITIVE), "positive");
    HU_ASSERT_STR_EQ(hu_feedback_type_str(HU_FEEDBACK_NEGATIVE), "negative");
    HU_ASSERT_STR_EQ(hu_feedback_type_str(HU_FEEDBACK_NEUTRAL), "neutral");
    HU_ASSERT_STR_EQ(hu_feedback_type_str(HU_FEEDBACK_CORRECTION), "correction");
}

static void test_feedback_signal_deinit_frees_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_feedback_signal_t fb = {
        .contact_id = hu_strndup(&alloc, "c1", 2),
        .contact_id_len = 2,
        .type = HU_FEEDBACK_POSITIVE,
        .context = hu_strndup(&alloc, "ctx", 3),
        .context_len = 3,
        .our_action = hu_strndup(&alloc, "act", 3),
        .our_action_len = 3,
        .timestamp = 0,
    };
    hu_feedback_signal_deinit(&alloc, &fb);
    HU_ASSERT_NULL(fb.contact_id);
    HU_ASSERT_NULL(fb.context);
    HU_ASSERT_NULL(fb.our_action);
}

static void test_reflection_entry_deinit_frees_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_reflection_entry_t entry = {
        .period = hu_strndup(&alloc, "daily", 5),
        .period_len = 5,
        .insights = hu_strndup(&alloc, "x", 1),
        .insights_len = 1,
        .improvements = hu_strndup(&alloc, "y", 1),
        .improvements_len = 1,
        .created_at = 0,
    };
    hu_reflection_entry_deinit(&alloc, &entry);
    HU_ASSERT_NULL(entry.period);
    HU_ASSERT_NULL(entry.insights);
    HU_ASSERT_NULL(entry.improvements);
}

static void test_skill_observation_deinit_frees_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_skill_observation_t obs = {
        .skill_name = hu_strndup(&alloc, "empathy", 7),
        .skill_name_len = 7,
        .contact_id = hu_strndup(&alloc, "c1", 2),
        .contact_id_len = 2,
        .proficiency = 0.5,
        .practice_count = 5,
        .last_practiced = 0,
    };
    hu_skill_observation_deinit(&alloc, &obs);
    HU_ASSERT_NULL(obs.skill_name);
    HU_ASSERT_NULL(obs.contact_id);
}

static void test_reflection_create_tables_sql_null_buf_returns_error(void) {
    size_t len = 0;
    hu_error_t err = hu_reflection_create_tables_sql(NULL, 1024, &len);
    HU_ASSERT_NEQ(err, HU_OK);
}

#ifdef HU_ENABLE_SQLITE
static void insert_behavioral_feedback(sqlite3 *db, const char *bt, const char *cid,
                                       const char *sig, const char *ctx, int64_t ts) {
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "INSERT INTO behavioral_feedback (behavior_type, contact_id, "
                                "signal, context, timestamp) VALUES (?1, ?2, ?3, ?4, ?5)",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return;
    sqlite3_bind_text(stmt, 1, bt, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, cid, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, sig, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, ctx, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, ts);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static void test_daily_reflection_three_positive_creates_skill(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    const char *bt = "response_style";
    const char *cid = "contact_a";
    const char *sig = "positive";
    const char *ctx = "warm reply";
    int64_t now = 1700100000;
    int64_t run_ts = now + 86400;

    insert_behavioral_feedback(db, bt, cid, sig, ctx, now + 1);
    insert_behavioral_feedback(db, bt, cid, sig, ctx, now + 2);
    insert_behavioral_feedback(db, bt, cid, sig, ctx, now + 3);

    sqlite3_stmt *chk = NULL;
    int chk_rc = sqlite3_prepare_v2(db,
        "SELECT COUNT(*) FROM behavioral_feedback WHERE signal='positive' AND timestamp > ?1",
        -1, &chk, NULL);
    HU_ASSERT_EQ(chk_rc, SQLITE_OK);
    sqlite3_bind_int64(chk, 1, now);
    chk_rc = sqlite3_step(chk);
    HU_ASSERT_EQ(chk_rc, SQLITE_ROW);
    int fb_count = sqlite3_column_int(chk, 0);
    sqlite3_finalize(chk);
    HU_ASSERT_EQ(fb_count, 3);

    hu_reflection_engine_t engine = {.alloc = &alloc, .db = db};
    hu_error_t err = hu_reflection_daily(&engine, run_ts);
    HU_ASSERT_EQ(err, HU_OK);

    sqlite3_stmt *sel = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT id, name, origin FROM skills WHERE origin='reflection'",
                                -1, &sel, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(sel);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    HU_ASSERT_TRUE(sqlite3_column_int64(sel, 0) > 0);
    HU_ASSERT_TRUE(strstr((const char *)sqlite3_column_text(sel, 1), "response_style") != NULL);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(sel, 2), "reflection");
    sqlite3_finalize(sel);

    mem.vtable->deinit(mem.ctx);
}

static void test_daily_reflection_no_feedback_no_skills(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t now = 1700100000;
    hu_reflection_engine_t engine = {.alloc = &alloc, .db = db};
    hu_error_t err = hu_reflection_daily(&engine, now);
    HU_ASSERT_EQ(err, HU_OK);

    sqlite3_stmt *sel = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM skills WHERE origin='reflection'",
                                -1, &sel, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(sel);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(sel, 0), 0);
    sqlite3_finalize(sel);

    mem.vtable->deinit(mem.ctx);
}

static void test_daily_reflection_summary_stored_in_kv(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t now = 1700200000;
    hu_reflection_engine_t engine = {.alloc = &alloc, .db = db};
    hu_error_t err = hu_reflection_daily(&engine, now);
    HU_ASSERT_EQ(err, HU_OK);

    sqlite3_stmt *sel = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT value FROM kv WHERE key='reflection_daily_last'",
                                -1, &sel, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    rc = sqlite3_step(sel);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(sel, 0), "1700200000");
    sqlite3_finalize(sel);

    mem.vtable->deinit(mem.ctx);
}
#endif /* HU_ENABLE_SQLITE */

void run_intelligence_reflection_tests(void) {
    HU_TEST_SUITE("intelligence_reflection");
    HU_RUN_TEST(test_reflection_create_tables_sql_valid);
    HU_RUN_TEST(test_feedback_insert_sql_valid);
    HU_RUN_TEST(test_feedback_insert_sql_escapes_quotes);
    HU_RUN_TEST(test_feedback_query_recent_sql_valid);
    HU_RUN_TEST(test_reflection_insert_sql_valid);
    HU_RUN_TEST(test_reflection_query_latest_sql_valid);
    HU_RUN_TEST(test_feedback_classify_left_on_read_negative);
    HU_RUN_TEST(test_feedback_classify_short_response_negative);
    HU_RUN_TEST(test_feedback_classify_contains_question_positive);
    HU_RUN_TEST(test_feedback_classify_otherwise_neutral);
    HU_RUN_TEST(test_skill_proficiency_score_formula);
    HU_RUN_TEST(test_cross_contact_learning_weight_formula);
    HU_RUN_TEST(test_reflection_build_prompt_with_feedback);
    HU_RUN_TEST(test_reflection_build_prompt_empty);
    HU_RUN_TEST(test_feedback_type_str_all_types);
    HU_RUN_TEST(test_feedback_signal_deinit_frees_all);
    HU_RUN_TEST(test_reflection_entry_deinit_frees_all);
    HU_RUN_TEST(test_skill_observation_deinit_frees_all);
    HU_RUN_TEST(test_reflection_create_tables_sql_null_buf_returns_error);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_daily_reflection_three_positive_creates_skill);
    HU_RUN_TEST(test_daily_reflection_no_feedback_no_skills);
    HU_RUN_TEST(test_daily_reflection_summary_stored_in_kv);
#endif
}
