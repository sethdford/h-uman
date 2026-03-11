#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/intelligence/skills.h"
#include "human/memory.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <string.h>
#include <time.h>

#define SL(s) (s), (sizeof(s) - 1)

static void ensure_skills_table(sqlite3 *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS skills ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "name TEXT NOT NULL,"
        "type TEXT NOT NULL,"
        "contact_id TEXT,"
        "trigger_conditions TEXT,"
        "strategy TEXT NOT NULL,"
        "success_rate REAL DEFAULT 0.5,"
        "attempts INTEGER DEFAULT 0,"
        "successes INTEGER DEFAULT 0,"
        "version INTEGER DEFAULT 1,"
        "origin TEXT NOT NULL,"
        "parent_skill_id INTEGER,"
        "created_at INTEGER NOT NULL,"
        "updated_at INTEGER,"
        "retired INTEGER DEFAULT 0)";
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static void ensure_skill_attempts_table(sqlite3 *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS skill_attempts("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "skill_id INTEGER NOT NULL,"
        "contact_id TEXT NOT NULL,"
        "applied_at INTEGER NOT NULL,"
        "outcome_signal TEXT,"
        "outcome_evidence TEXT,"
        "context TEXT)";
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static void ensure_skill_evolution_table(sqlite3 *db) {
    const char *sql =
        "CREATE TABLE IF NOT EXISTS skill_evolution("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "skill_id INTEGER NOT NULL,"
        "version INTEGER NOT NULL,"
        "strategy TEXT NOT NULL,"
        "success_rate REAL,"
        "evolved_at INTEGER NOT NULL,"
        "reason TEXT)";
    sqlite3_exec(db, sql, NULL, NULL, NULL);
}

static void test_skill_insert_load_active_by_contact_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_skills_table(db);

    int64_t now = (int64_t)time(NULL);
    int64_t out_id = 0;
    hu_error_t err = hu_skill_insert(&alloc, db,
                                     "comfort_mindy", 13,
                                     "interpersonal", 13,
                                     "contact_a", 9,
                                     "emotion==sad", 12,
                                     "Acknowledge first. Short messages.", 34,
                                     "reflection", 10,
                                     0, now, &out_id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out_id > 0);

    hu_skill_t *skills = NULL;
    size_t count = 0;
    err = hu_skill_load_active(&alloc, db, "contact_a", 9, &skills, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(skills);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(skills[0].name, "comfort_mindy");
    HU_ASSERT_STR_EQ(skills[0].type, "interpersonal");
    HU_ASSERT_STR_EQ(skills[0].contact_id, "contact_a");
    HU_ASSERT_STR_EQ(skills[0].strategy, "Acknowledge first. Short messages.");
    hu_skill_free(&alloc, skills, count);

    mem.vtable->deinit(mem.ctx);
}

static void test_skill_load_null_contact_returns_universal_only(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_skills_table(db);

    int64_t now = (int64_t)time(NULL);
    int64_t out_id = 0;
    hu_error_t err = hu_skill_insert(&alloc, db,
                                     "universal_skill", 15,
                                     "generic", 7,
                                     NULL, 0,
                                     NULL, 0,
                                     "Universal strategy.", 19,
                                     "manual", 6,
                                     0, now, &out_id);
    HU_ASSERT_EQ(err, HU_OK);

    hu_skill_t *skills = NULL;
    size_t count = 0;
    err = hu_skill_load_active(&alloc, db, NULL, 0, &skills, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(skills);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(skills[0].name, "universal_skill");
    HU_ASSERT_EQ(skills[0].contact_id[0], '\0');
    hu_skill_free(&alloc, skills, count);

    mem.vtable->deinit(mem.ctx);
}

static void test_skill_insert_with_parent_skill_id_stored(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_skills_table(db);

    int64_t now = (int64_t)time(NULL);
    int64_t parent_id = 0;
    hu_error_t err = hu_skill_insert(&alloc, db,
                                     "parent_skill", 12,
                                     "base", 4,
                                     NULL, 0,
                                     NULL, 0,
                                     "Parent strategy.", 16,
                                     "manual", 6,
                                     0, now, &parent_id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(parent_id > 0);

    int64_t child_id = 0;
    err = hu_skill_insert(&alloc, db,
                          "child_skill", 11,
                          "derived", 7,
                          NULL, 0,
                          NULL, 0,
                          "Child strategy.", 15,
                          "manual", 6,
                          parent_id, now, &child_id);
    HU_ASSERT_EQ(err, HU_OK);

    hu_skill_t out = {0};
    err = hu_skill_get_by_name(&alloc, db, "child_skill", 11, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.parent_skill_id, parent_id);

    mem.vtable->deinit(mem.ctx);
}

static void test_match_triggers_emotion_contact_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_skills_table(db);

    int64_t now = (int64_t)time(NULL);
    int64_t out_id = 0;
    hu_error_t err = hu_skill_insert(&alloc, db,
                                     "comfort_user_a", 14,
                                     "interpersonal", 13,
                                     "user_a", 6,
                                     "emotion==sad,contact==user_a", 28,
                                     "Acknowledge first.", 18,
                                     "reflection", 10,
                                     0, now, &out_id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out_id > 0);

    hu_skill_t *matched = NULL;
    size_t count = 0;
    err = hu_skill_match_triggers(&alloc, db,
                                  "user_a", 6,
                                  "sad", 3,
                                  NULL, 0,
                                  0.9,
                                  &matched, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(matched);
    HU_ASSERT_EQ(count, 1u);
    HU_ASSERT_STR_EQ(matched[0].name, "comfort_user_a");
    hu_skill_free(&alloc, matched, count);

    mem.vtable->deinit(mem.ctx);
}

static void test_match_triggers_confidence_below_threshold_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_skills_table(db);

    int64_t now = (int64_t)time(NULL);
    int64_t out_id = 0;
    hu_error_t err = hu_skill_insert(&alloc, db,
                                     "high_conf_skill", 15,
                                     "test", 4,
                                     NULL, 0,
                                     "emotion==sad,confidence>=0.8", 28,
                                     "Strategy.", 9,
                                     "manual", 6,
                                     0, now, &out_id);
    HU_ASSERT_EQ(err, HU_OK);

    hu_skill_t *matched = NULL;
    size_t count = 0;
    err = hu_skill_match_triggers(&alloc, db,
                                  "user_a", 6,
                                  "sad", 3,
                                  NULL, 0,
                                  0.5,
                                  &matched, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(matched == NULL || count == 0);
    if (matched)
        hu_skill_free(&alloc, matched, count);

    mem.vtable->deinit(mem.ctx);
}

static void test_record_attempt_update_success_rate_verified(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_skills_table(db);
    ensure_skill_attempts_table(db);

    int64_t now = (int64_t)time(NULL);
    int64_t skill_id = 0;
    hu_error_t err = hu_skill_insert(&alloc, db,
                                     "recorded_skill", 14,
                                     "test", 4,
                                     "user_a", 6,
                                     NULL, 0,
                                     "Strategy.", 9,
                                     "manual", 6,
                                     0, now, &skill_id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(skill_id > 0);

    int64_t attempt_id = 0;
    err = hu_skill_record_attempt(db,
                                  skill_id, "user_a", 6,
                                  now,
                                  "success", 7,
                                  "User responded positively.", 26,
                                  "test context", 12,
                                  &attempt_id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(attempt_id > 0);

    err = hu_skill_update_success_rate(db, skill_id, 1, 1);
    HU_ASSERT_EQ(err, HU_OK);

    hu_skill_t out = {0};
    err = hu_skill_get_by_name(&alloc, db, "recorded_skill", 14, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.attempts, 1);
    HU_ASSERT_EQ(out.successes, 1);
    HU_ASSERT_EQ(out.success_rate, 1.0);

    mem.vtable->deinit(mem.ctx);
}

static void test_evolve_skill_version_incremented_evolution_row_inserted(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_skills_table(db);
    ensure_skill_evolution_table(db);

    int64_t now = (int64_t)time(NULL);
    int64_t skill_id = 0;
    hu_error_t err = hu_skill_insert(&alloc, db,
                                     "evolve_skill", 12,
                                     "test", 4,
                                     NULL, 0,
                                     NULL, 0,
                                     "Old strategy.", 13,
                                     "manual", 6,
                                     0, now, &skill_id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(skill_id > 0);

    err = hu_skill_evolve(&alloc, db, skill_id,
                          "New improved strategy.", 22,
                          "Low success rate", 16,
                          now + 1);
    HU_ASSERT_EQ(err, HU_OK);

    hu_skill_t out = {0};
    err = hu_skill_get_by_name(&alloc, db, "evolve_skill", 12, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.version, 2);
    HU_ASSERT_STR_EQ(out.strategy, "New improved strategy.");
    HU_ASSERT_EQ(out.success_rate, 0.5);
    HU_ASSERT_EQ(out.attempts, 0);
    HU_ASSERT_EQ(out.successes, 0);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT version, strategy, success_rate FROM skill_evolution WHERE skill_id=?",
        -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, skill_id);
    rc = sqlite3_step(stmt);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    HU_ASSERT_EQ(sqlite3_column_int(stmt, 0), 1);
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "Old strategy.");
    sqlite3_finalize(stmt);

    mem.vtable->deinit(mem.ctx);
}

static void test_should_retire_version_3_rate_0_3_returns_true(void) {
    HU_ASSERT_TRUE(hu_skill_db_should_retire(3, 0.3));
}

static void test_should_retire_version_2_rate_0_3_returns_false(void) {
    HU_ASSERT_FALSE(hu_skill_db_should_retire(2, 0.3));
}

static void test_retire_skill_excluded_from_load_active(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_skills_table(db);

    int64_t now = (int64_t)time(NULL);
    int64_t skill_id = 0;
    hu_error_t err = hu_skill_insert(&alloc, db,
                                     "retire_me", 9,
                                     "test", 4,
                                     "user_a", 6,
                                     NULL, 0,
                                     "Strategy.", 9,
                                     "manual", 6,
                                     0, now, &skill_id);
    HU_ASSERT_EQ(err, HU_OK);

    hu_skill_t *skills = NULL;
    size_t count = 0;
    err = hu_skill_load_active(&alloc, db, "user_a", 6, &skills, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 1u);
    hu_skill_free(&alloc, skills, count);

    err = hu_skill_retire(db, skill_id);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_skill_load_active(&alloc, db, "user_a", 6, &skills, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(count, 0u);

    mem.vtable->deinit(mem.ctx);
}

static void test_transfer_skill_creates_universal_with_parent_skill_id(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_skills_table(db);

    int64_t now = (int64_t)time(NULL);
    int64_t orig_id = 0;
    hu_error_t err = hu_skill_insert(&alloc, db,
                                     "contact_specific", 16,
                                     "interpersonal", 13,
                                     "user_a", 6,
                                     "emotion==sad", 12,
                                     SL("Be gentle with this person."),
                                     "reflection", 10,
                                     0, now, &orig_id);
    HU_ASSERT_EQ(err, HU_OK);

    int64_t new_id = 0;
    err = hu_skill_transfer(&alloc, db, orig_id,
                            "emotion==sad", 12,
                            0.1, now + 1, &new_id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(new_id > 0);
    HU_ASSERT_NEQ(new_id, orig_id);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
        "SELECT contact_id, trigger_conditions, parent_skill_id, strategy "
        "FROM skills WHERE id=?",
        -1, &stmt, NULL);
    HU_ASSERT_EQ(rc, SQLITE_OK);
    sqlite3_bind_int64(stmt, 1, new_id);
    rc = sqlite3_step(stmt);
    HU_ASSERT_EQ(rc, SQLITE_ROW);
    HU_ASSERT_NULL(sqlite3_column_text(stmt, 0));
    HU_ASSERT_STR_EQ((const char *)sqlite3_column_text(stmt, 1), "emotion==sad");
    HU_ASSERT_EQ(sqlite3_column_int64(stmt, 2), orig_id);
    const char *strat = (const char *)sqlite3_column_text(stmt, 3);
    HU_ASSERT_TRUE(strstr(strat, "Generalized: ") == strat);
    HU_ASSERT_TRUE(strstr(strat, "Be gentle with this person.") != NULL);
    sqlite3_finalize(stmt);

    mem.vtable->deinit(mem.ctx);
}

static void test_resolve_chain_skill_basics_expanded_includes_basics_strategy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_skills_table(db);

    int64_t now = (int64_t)time(NULL);
    int64_t basics_id = 0;
    hu_error_t err = hu_skill_insert(&alloc, db,
                                     "basics", 6,
                                     "base", 4,
                                     NULL, 0,
                                     NULL, 0,
                                     SL("Listen and acknowledge."),
                                     "manual", 6,
                                     0, now, &basics_id);
    HU_ASSERT_EQ(err, HU_OK);

    int64_t advanced_id = 0;
    err = hu_skill_insert(&alloc, db,
                          "advanced", 8,
                          "derived", 7,
                          NULL, 0,
                          NULL, 0,
                          SL("First apply skill:basics then add nuance."),
                          SL("manual"),
                          0, now, &advanced_id);
    HU_ASSERT_EQ(err, HU_OK);

    char out[2048];
    size_t out_len = 0;
    err = hu_skill_resolve_chain(&alloc, db,
                                 SL("First apply skill:basics then add nuance."),
                                 out, sizeof(out), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(out, "Listen and acknowledge.") != NULL);
    HU_ASSERT_TRUE(strstr(out, "then add nuance.") != NULL);

    mem.vtable->deinit(mem.ctx);
}

static void test_resolve_chain_depth_gt_3_stops_recursion(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_skills_table(db);

    int64_t now = (int64_t)time(NULL);
    int64_t id = 0;
    hu_error_t err = hu_skill_insert(&alloc, db,
                                    "a", 1, "chain", 5, NULL, 0, NULL, 0,
                                    "A content.", 10, "manual", 6, 0, now, &id);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_skill_insert(&alloc, db,
                          "b", 1, "chain", 5, NULL, 0, NULL, 0,
                          "B skill:a", 9, "manual", 6, 0, now, &id);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_skill_insert(&alloc, db,
                          "c", 1, "chain", 5, NULL, 0, NULL, 0,
                          "C skill:b", 9, "manual", 6, 0, now, &id);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_skill_insert(&alloc, db,
                          "d", 1, "chain", 5, NULL, 0, NULL, 0,
                          "D skill:c", 9, "manual", 6, 0, now, &id);
    HU_ASSERT_EQ(err, HU_OK);

    err = hu_skill_insert(&alloc, db,
                          "e", 1, "chain", 5, NULL, 0, NULL, 0,
                          "E skill:d", 9, "manual", 6, 0, now, &id);
    HU_ASSERT_EQ(err, HU_OK);

    char out[2048];
    size_t out_len = 0;
    err = hu_skill_resolve_chain(&alloc, db,
                                 SL("Start skill:e end."),
                                 out, sizeof(out), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "skill:a") != NULL);
    HU_ASSERT_TRUE(strstr(out, "E ") != NULL);

    mem.vtable->deinit(mem.ctx);
}

static void test_skill_get_by_name_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    ensure_skills_table(db);

    int64_t now = (int64_t)time(NULL);
    int64_t out_id = 0;
    hu_error_t err = hu_skill_insert(&alloc, db,
                                     "named_skill", 11,
                                     "test", 4,
                                     "c1", 2,
                                     "cond", 4,
                                     "Do something.", 13,
                                     "reflection", 10,
                                     0, now, &out_id);
    HU_ASSERT_EQ(err, HU_OK);

    hu_skill_t out = {0};
    err = hu_skill_get_by_name(&alloc, db, "named_skill", 11, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.id, out_id);
    HU_ASSERT_STR_EQ(out.name, "named_skill");
    HU_ASSERT_STR_EQ(out.type, "test");
    HU_ASSERT_STR_EQ(out.contact_id, "c1");
    HU_ASSERT_STR_EQ(out.strategy, "Do something.");
    HU_ASSERT_STR_EQ(out.origin, "reflection");

    mem.vtable->deinit(mem.ctx);
}

void run_intelligence_skills_tests(void) {
    HU_TEST_SUITE("intelligence_skills");
    HU_RUN_TEST(test_skill_insert_load_active_by_contact_found);
    HU_RUN_TEST(test_skill_load_null_contact_returns_universal_only);
    HU_RUN_TEST(test_skill_insert_with_parent_skill_id_stored);
    HU_RUN_TEST(test_skill_get_by_name_found);
    HU_RUN_TEST(test_match_triggers_emotion_contact_found);
    HU_RUN_TEST(test_match_triggers_confidence_below_threshold_not_found);
    HU_RUN_TEST(test_record_attempt_update_success_rate_verified);
    HU_RUN_TEST(test_evolve_skill_version_incremented_evolution_row_inserted);
    HU_RUN_TEST(test_should_retire_version_3_rate_0_3_returns_true);
    HU_RUN_TEST(test_should_retire_version_2_rate_0_3_returns_false);
    HU_RUN_TEST(test_retire_skill_excluded_from_load_active);
    HU_RUN_TEST(test_transfer_skill_creates_universal_with_parent_skill_id);
    HU_RUN_TEST(test_resolve_chain_skill_basics_expanded_includes_basics_strategy);
    HU_RUN_TEST(test_resolve_chain_depth_gt_3_stops_recursion);
}

#else

void run_intelligence_skills_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
