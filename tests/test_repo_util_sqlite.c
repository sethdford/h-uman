/* Exercises hu_repo_exec_ddl in src/memory/repos/repo_util_sqlite.c — the
 * shared DDL helper #436 extracted from contact_optout_repo_sqlite.c and
 * proactive_decisions_repo_sqlite.c.
 *
 * Every assertion here is a pre/post contract against sqlite_master rather
 * than a check of the return code alone: a helper that returned HU_OK without
 * executing the DDL would pass a return-code-only test and still break every
 * repo that depends on it for schema creation. */
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include "human/memory/repo_util.h"
#include "test_framework.h"
#include <sqlite3.h>

/* Count rows in sqlite_master for `name` — 0 before the DDL, 1 after. */
static int64_t table_count(sqlite3 *db, const char *name) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' AND name=?",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, name, -1, SQLITE_TRANSIENT);
    int64_t n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int64(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}

static void test_repo_exec_ddl_creates_the_table(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    /* Pre: the table does not exist. */
    HU_ASSERT_EQ(table_count(db, "hu_ddl_probe"), 0);

    HU_ASSERT_EQ(hu_repo_exec_ddl(db, "CREATE TABLE hu_ddl_probe (id INTEGER PRIMARY KEY, v TEXT)"),
                 HU_OK);

    /* Post: it does — the DDL actually ran, not just returned HU_OK. */
    HU_ASSERT_EQ(table_count(db, "hu_ddl_probe"), 1);

    /* And the created schema is usable. */
    HU_ASSERT_EQ(hu_repo_exec_ddl(db, "INSERT INTO hu_ddl_probe (v) VALUES ('x')"), HU_OK);

    mem.vtable->deinit(mem.ctx);
}

static void test_repo_exec_ddl_reports_bad_sql_and_leaves_schema_alone(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    /* Malformed DDL maps to HU_ERR_MEMORY_STORE (errmsg freed, not leaked —
     * this case is why the helper exists rather than raw sqlite3_exec). */
    HU_ASSERT_EQ(hu_repo_exec_ddl(db, "CREATE TABLE ((("), HU_ERR_MEMORY_STORE);

    /* A failed CREATE must not half-create the table. */
    HU_ASSERT_EQ(hu_repo_exec_ddl(db, "CREATE TABLE hu_ddl_bad (id INTEGER"), HU_ERR_MEMORY_STORE);
    HU_ASSERT_EQ(table_count(db, "hu_ddl_bad"), 0);

    mem.vtable->deinit(mem.ctx);
}

static void test_repo_exec_ddl_rejects_bad_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);

    HU_ASSERT_EQ(hu_repo_exec_ddl(NULL, "CREATE TABLE t (id INTEGER)"), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_repo_exec_ddl(db, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_repo_exec_ddl(NULL, NULL), HU_ERR_INVALID_ARGUMENT);

    mem.vtable->deinit(mem.ctx);
}

static void test_repo_step_insert_tells_new_from_already_there(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_repo_exec_ddl(db, "CREATE TABLE t (k TEXT PRIMARY KEY);"), HU_OK);
    sqlite3_stmt *st = NULL;
    bool inserted = false;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, "INSERT INTO t VALUES ('a');", -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(hu_repo_step_insert(st, &inserted), HU_OK);
    HU_ASSERT_TRUE(inserted);
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, "INSERT INTO t VALUES ('a');", -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(hu_repo_step_insert(st, &inserted), HU_OK); /* duplicate: an answer */
    HU_ASSERT_FALSE(inserted);
    /* A failure that is not a constraint is an error. */
    HU_ASSERT_EQ(
        sqlite3_prepare_v2(db, "INSERT INTO t SELECT abs(-9223372036854775807-1);", -1, &st, NULL),
        SQLITE_OK);
    HU_ASSERT_EQ(hu_repo_step_insert(st, &inserted), HU_ERR_MEMORY_STORE);
    HU_ASSERT_EQ(hu_repo_step_insert(NULL, &inserted), HU_ERR_INVALID_ARGUMENT);
    sqlite3_close(db);
}

static void test_repo_step_update_one_distinguishes_no_row(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(hu_repo_exec_ddl(db, "CREATE TABLE t (k TEXT, v INTEGER);"
                                      "INSERT INTO t VALUES ('a', 1);"),
                 HU_OK);
    sqlite3_stmt *st = NULL;
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, "UPDATE t SET v=2 WHERE k='a';", -1, &st, NULL), SQLITE_OK);
    HU_ASSERT_EQ(hu_repo_step_update_one(db, st), HU_OK);
    HU_ASSERT_EQ(sqlite3_prepare_v2(db, "UPDATE t SET v=3 WHERE k='zz';", -1, &st, NULL),
                 SQLITE_OK);
    HU_ASSERT_EQ(hu_repo_step_update_one(db, st), HU_ERR_NOT_FOUND);
    HU_ASSERT_EQ(hu_repo_step_update_one(NULL, NULL), HU_ERR_INVALID_ARGUMENT);
    sqlite3_close(db);
}

void run_repo_util_sqlite_tests(void) {
    HU_RUN_TEST(test_repo_exec_ddl_creates_the_table);
    HU_RUN_TEST(test_repo_step_insert_tells_new_from_already_there);
    HU_RUN_TEST(test_repo_step_update_one_distinguishes_no_row);
    HU_RUN_TEST(test_repo_exec_ddl_reports_bad_sql_and_leaves_schema_alone);
    HU_RUN_TEST(test_repo_exec_ddl_rejects_bad_args);
}
#else
void run_repo_util_sqlite_tests(void) {}
#endif
