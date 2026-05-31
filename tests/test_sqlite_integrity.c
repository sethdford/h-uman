/* tests/test_sqlite_integrity.c
 *
 * Resilience: a corrupted on-disk memory DB must SELF-HEAL (quarantine the bad
 * bytes + reopen fresh) rather than be read as silent garbage. Pins the contract
 * of hu_sqlite_quick_check_ok / hu_sqlite_quarantine_corrupt_file and the
 * self-heal path in hu_sqlite_memory_create (src/memory/engines/sqlite.c).
 */
#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/sql_common.h"
#include "test_framework.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* quick_check returns true on a freshly-created, valid db. */
static void test_quick_check_ok_on_valid_db(void) {
    sqlite3 *db = NULL;
    HU_ASSERT_EQ(sqlite3_open(":memory:", &db), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_exec(db, "CREATE TABLE t(x);", NULL, NULL, NULL), SQLITE_OK);
    HU_ASSERT_TRUE(hu_sqlite_quick_check_ok(db));
    sqlite3_close(db);
}

/* NULL handle -> false (never claims a missing db is healthy). */
static void test_quick_check_false_on_null(void) {
    HU_ASSERT_FALSE(hu_sqlite_quick_check_ok(NULL));
}

/* quarantine renames the file aside; no-op (false) for :memory:/NULL/empty. */
static void test_quarantine_moves_corrupt_file_aside(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_sqlite_integrity_q_%d.db", (int)getpid());
    remove(path);
    FILE *f = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(f);
    fwrite("not a database", 1, 14, f);
    fclose(f);

    HU_ASSERT_TRUE(hu_sqlite_quarantine_corrupt_file(path));
    HU_ASSERT_TRUE(access(path, F_OK) != 0); /* original renamed aside */

    HU_ASSERT_FALSE(hu_sqlite_quarantine_corrupt_file(":memory:"));
    HU_ASSERT_FALSE(hu_sqlite_quarantine_corrupt_file(NULL));
    HU_ASSERT_FALSE(hu_sqlite_quarantine_corrupt_file(""));
    /* quarantined sibling left in /tmp (ephemeral); OS reclaims it. */
}

/* END-TO-END: a corrupt on-disk DB is detected, quarantined, and replaced by a
 * fresh WORKING db -- not read as garbage and not refused. Non-vacuous: asserts
 * the file was corrupt before (the bug condition) and valid+live after (the fix). */
static void test_create_self_heals_corrupt_db(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_sqlite_integrity_heal_%d.db", (int)getpid());
    remove(path);

    /* Write garbage where a valid DB should be. */
    FILE *f = fopen(path, "wb");
    HU_ASSERT_NOT_NULL(f);
    for (int i = 0; i < 256; i++)
        fputc(0xAB, f);
    fclose(f);

    /* Pre: raw open succeeds (lazy) but quick_check confirms corruption. */
    sqlite3 *raw = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &raw), SQLITE_OK);
    HU_ASSERT_FALSE(hu_sqlite_quick_check_ok(raw));
    sqlite3_close(raw);

    /* Act: the production create path must self-heal, not serve garbage. */
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, path);
    HU_ASSERT_NOT_NULL(mem.vtable); /* healed to a live engine */
    HU_ASSERT_NOT_NULL(mem.ctx);

    /* Post: the db at `path` is now a VALID sqlite db (fresh, passes quick_check). */
    raw = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &raw), SQLITE_OK);
    HU_ASSERT_TRUE(hu_sqlite_quick_check_ok(raw));
    sqlite3_close(raw);

    if (mem.vtable && mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    remove(path);
}

void run_sqlite_integrity_tests(void) {
    HU_TEST_SUITE("sqlite_integrity");
    HU_RUN_TEST(test_quick_check_ok_on_valid_db);
    HU_RUN_TEST(test_quick_check_false_on_null);
    HU_RUN_TEST(test_quarantine_moves_corrupt_file_aside);
    HU_RUN_TEST(test_create_self_heals_corrupt_db);
}

#else
void run_sqlite_integrity_tests(void) { (void)0; }
#endif
