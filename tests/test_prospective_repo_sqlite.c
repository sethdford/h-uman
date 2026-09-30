/* tests/test_prospective_repo_sqlite.c
 *
 * Prospective memory v2 typed store (spec
 * docs/superpowers/specs/2026-09-30-prospective-memory-v2-design.md §4.1).
 * Task 1 pins the additive migration: v2 columns on a pre-v2 table, the
 * fired -> status mapping for existing rows, the trigger that keeps status in
 * step when a legacy writer (the nightly curator) changes fired, idempotence,
 * and that every engine-opened database is migrated. */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/prospective_repo.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

/* The table exactly as src/memory/engines/sqlite.c created it before v2. */
static sqlite3 *legacy_db(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_exec(db,
                              "CREATE TABLE prospective_memories(id INTEGER PRIMARY KEY "
                              "AUTOINCREMENT,trigger_type TEXT NOT NULL,trigger_value TEXT NOT "
                              "NULL,action TEXT NOT NULL,contact_id TEXT,expires_at INTEGER,fired "
                              "INTEGER DEFAULT 0,created_at INTEGER NOT NULL)",
                              NULL, NULL, NULL),
                 SQLITE_OK);
    return db;
}

static void legacy_insert(sqlite3 *db, const char *action, int fired) {
    char sql[256];
    snprintf(sql, sizeof(sql),
             "INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
             "expires_at,fired,created_at) VALUES('keyword','kw','%s','+15550000001',0,%d,100)",
             action, fired);
    HU_ASSERT_EQ(sqlite3_exec(db, sql, NULL, NULL, NULL), SQLITE_OK);
}

static int64_t q_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    int64_t v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

static void q_text(sqlite3 *db, const char *sql, char *buf, size_t cap) {
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, sql, -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_step(st), SQLITE_ROW);
    const unsigned char *t = sqlite3_column_text(st, 0);
    snprintf(buf, cap, "%s", t ? (const char *)t : "(null)");
    sqlite3_finalize(st);
}

static int64_t column_count(sqlite3 *db) {
    return q_int(db, "SELECT COUNT(*) FROM pragma_table_info('prospective_memories')");
}

static void ensure_schema_adds_typed_columns_and_maps_fired(void) {
    sqlite3 *db = legacy_db();
    legacy_insert(db, "open", 0);
    legacy_insert(db, "fired", 1);
    legacy_insert(db, "pruned", 2);
    legacy_insert(db, "expired", 3);
    HU_ASSERT_EQ(column_count(db), (int64_t)8);

    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_OK);

    HU_ASSERT_EQ(column_count(db), (int64_t)15);
    static const char *const cols[] = {"cue_kind", "due_at",  "status", "surfaced_at",
                                       "attempts", "outcome", "source"};
    for (size_t i = 0; i < sizeof(cols) / sizeof(cols[0]); i++) {
        char sql[160];
        snprintf(sql, sizeof(sql),
                 "SELECT COUNT(*) FROM pragma_table_info('prospective_memories') WHERE name='%s'",
                 cols[i]);
        HU_ASSERT_EQ(q_int(db, sql), (int64_t)1);
    }
    char s[32];
    q_text(db, "SELECT status FROM prospective_memories WHERE action='open'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending");
    q_text(db, "SELECT status FROM prospective_memories WHERE action='fired'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "done");
    q_text(db, "SELECT status FROM prospective_memories WHERE action='pruned'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "canceled");
    q_text(db, "SELECT status FROM prospective_memories WHERE action='expired'", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "expired");
    /* existing rows get the defaults for the other columns */
    q_text(db, "SELECT cue_kind || '/' || source FROM prospective_memories WHERE action='open'", s,
           sizeof(s));
    HU_ASSERT_STR_EQ(s, "keyword/extractor");
    HU_ASSERT_EQ(q_int(db, "SELECT attempts FROM prospective_memories WHERE action='open'"),
                 (int64_t)0);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM prospective_memories WHERE due_at IS NOT NULL"),
                 (int64_t)0);
    sqlite3_close(db);
}

/* The curator (scripts/insight_stream.py) writes only the legacy columns:
 * its inserts must land as pending keyword rows, and its fired updates must
 * move status with them. */
static void legacy_writers_keep_status_in_step(void) {
    sqlite3 *db = legacy_db();
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_OK);
    legacy_insert(db, "curator row", 0);
    char s[32];
    q_text(db, "SELECT status || '/' || cue_kind FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "pending/keyword");

    HU_ASSERT_EQ(sqlite3_exec(db, "UPDATE prospective_memories SET fired=2", NULL, NULL, NULL),
                 SQLITE_OK);
    q_text(db, "SELECT status FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "canceled");
    HU_ASSERT_EQ(sqlite3_exec(db, "UPDATE prospective_memories SET fired=3", NULL, NULL, NULL),
                 SQLITE_OK);
    q_text(db, "SELECT status FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "expired");
    HU_ASSERT_EQ(sqlite3_exec(db, "UPDATE prospective_memories SET fired=1", NULL, NULL, NULL),
                 SQLITE_OK);
    q_text(db, "SELECT status FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "done");
    /* a v2 write of status alone (fired unchanged) is not overridden */
    HU_ASSERT_EQ(sqlite3_exec(db, "UPDATE prospective_memories SET fired=0, status='pending'", NULL,
                              NULL, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(
        sqlite3_exec(db, "UPDATE prospective_memories SET status='surfaced'", NULL, NULL, NULL),
        SQLITE_OK);
    q_text(db, "SELECT status FROM prospective_memories", s, sizeof(s));
    HU_ASSERT_STR_EQ(s, "surfaced");
    sqlite3_close(db);
}

static void ensure_schema_is_idempotent(void) {
    sqlite3 *db = legacy_db();
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_OK);
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_OK);
    HU_ASSERT_EQ(column_count(db), (int64_t)15);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='trigger' AND "
                           "name='trg_prospective_fired_status'"),
                 (int64_t)1);
    HU_ASSERT_EQ(q_int(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND "
                           "name='idx_prospective_status'"),
                 (int64_t)1);
    sqlite3_close(db);
}

static void ensure_schema_refuses_a_missing_table_and_null(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(db), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_prospective_repo_ensure_schema(NULL), HU_ERR_INVALID_ARGUMENT);
    sqlite3_close(db);
}

/* The daemon never calls ensure_schema itself: the engine does at open. */
static void engine_open_migrates_prospective_memories(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);
    HU_ASSERT_EQ(column_count(db), (int64_t)15);
    mem.vtable->deinit(mem.ctx);
}

void run_prospective_repo_sqlite_tests(void) {
    HU_TEST_SUITE("prospective repo");
    HU_RUN_TEST(ensure_schema_adds_typed_columns_and_maps_fired);
    HU_RUN_TEST(legacy_writers_keep_status_in_step);
    HU_RUN_TEST(ensure_schema_is_idempotent);
    HU_RUN_TEST(ensure_schema_refuses_a_missing_table_and_null);
    HU_RUN_TEST(engine_open_migrates_prospective_memories);
}

#else

void run_prospective_repo_sqlite_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
