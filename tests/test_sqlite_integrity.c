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
#include <fcntl.h>
#include <glob.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Create a test scratch file with restrictive 0600 perms (not fopen's default
 * 0666 — CodeQL cpp/world-writable-file, CWE-732). Returns a FILE* or NULL. */
static FILE *open_private_wb(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return NULL;
    return fdopen(fd, "wb");
}

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
    FILE *f = open_private_wb(path);
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
    FILE *f = open_private_wb(path);
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

/* ===== Unclean-shutdown sentinel gating of the startup quick_check =====
 * PRAGMA quick_check walks every btree page — minutes of pread on a multi-GB
 * DB. The full scan must only run when the previous process did NOT close the
 * DB cleanly (sentinel file left behind). These tests pin the sentinel helper
 * contract and both branches of the gate. */

/* Count <db_path>.corrupt-* quarantine siblings (proof the heal path ran). */
static int count_quarantine_files(const char *db_path) {
    char pat[300];
    snprintf(pat, sizeof(pat), "%s.corrupt-*", db_path);
    glob_t g;
    int n = 0;
    if (glob(pat, 0, NULL, &g) == 0)
        n = (int)g.gl_pathc;
    globfree(&g);
    return n;
}

static void remove_db_and_siblings(const char *db_path) {
    char pat[300];
    snprintf(pat, sizeof(pat), "%s*", db_path);
    glob_t g;
    if (glob(pat, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++)
            remove(g.gl_pathv[i]);
    }
    globfree(&g);
}

/* Build a valid multi-page DB, then zero its FINAL page: sqlite3_open and
 * schema init still succeed (the sqlite_master btree lives in the early
 * pages) but PRAGMA quick_check fails on the mangled leaf. This is the
 * corruption class only the full scan can see. */
static bool make_interior_corrupt_db(const char *path) {
    remove_db_and_siblings(path);
    sqlite3 *db = NULL;
    if (sqlite3_open(path, &db) != SQLITE_OK)
        return false;
    int rc = sqlite3_exec(db,
                          "PRAGMA journal_mode=DELETE;"
                          "CREATE TABLE big(x TEXT);"
                          "WITH RECURSIVE c(i) AS (SELECT 1 UNION ALL SELECT i+1 FROM c "
                          "WHERE i<2000) "
                          "INSERT INTO big SELECT printf('%0128d', i) FROM c;",
                          NULL, NULL, NULL);
    long page_size = 0;
    sqlite3_stmt *stmt = NULL;
    if (rc == SQLITE_OK &&
        sqlite3_prepare_v2(db, "PRAGMA page_size;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW)
            page_size = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    if (rc != SQLITE_OK || page_size <= 0)
        return false;

    FILE *f = fopen(path, "r+b");
    if (!f)
        return false;
    if (fseek(f, -page_size, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    char *zeros = calloc(1, (size_t)page_size);
    bool ok = zeros && fwrite(zeros, 1, (size_t)page_size, f) == (size_t)page_size;
    free(zeros);
    fclose(f);
    return ok;
}

/* Sentinel helpers: absent by default, present after write, absent after
 * remove; no-ops (false) for paths with nothing on disk. */
static void test_open_sentinel_write_present_remove_roundtrip(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_sqlite_integrity_sent_%d.db", (int)getpid());
    remove_db_and_siblings(path);

    HU_ASSERT_FALSE(hu_sqlite_open_sentinel_present(path));
    HU_ASSERT_TRUE(hu_sqlite_open_sentinel_write(path));
    HU_ASSERT_TRUE(hu_sqlite_open_sentinel_present(path));
    hu_sqlite_open_sentinel_remove(path);
    HU_ASSERT_FALSE(hu_sqlite_open_sentinel_present(path));

    HU_ASSERT_FALSE(hu_sqlite_open_sentinel_present(NULL));
    HU_ASSERT_FALSE(hu_sqlite_open_sentinel_present(""));
    HU_ASSERT_FALSE(hu_sqlite_open_sentinel_present(":memory:"));
    HU_ASSERT_FALSE(hu_sqlite_open_sentinel_write(NULL));
    HU_ASSERT_FALSE(hu_sqlite_open_sentinel_write(":memory:"));
    remove_db_and_siblings(path);
}

/* The engine marks the DB open-on-disk and clears the mark on clean close, so
 * a crash between the two leaves the sentinel behind for the next boot. */
static void test_create_writes_sentinel_and_deinit_removes_it(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_sqlite_integrity_slc_%d.db", (int)getpid());
    remove_db_and_siblings(path);

    hu_memory_t mem = hu_sqlite_memory_create(&alloc, path);
    HU_ASSERT_NOT_NULL(mem.vtable);
    HU_ASSERT_TRUE(hu_sqlite_open_sentinel_present(path));

    mem.vtable->deinit(mem.ctx);
    HU_ASSERT_FALSE(hu_sqlite_open_sentinel_present(path));
    remove_db_and_siblings(path);
}

/* Clean-shutdown fast path: NO sentinel -> the multi-minute full-page scan is
 * skipped, so interior corruption (invisible to open + schema init) is NOT
 * quarantined and boot proceeds immediately. */
static void test_create_skips_full_check_after_clean_shutdown(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_sqlite_integrity_clean_%d.db", (int)getpid());
    HU_ASSERT_TRUE(make_interior_corrupt_db(path));

    /* Pre: this corruption IS the class quick_check catches. */
    sqlite3 *raw = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &raw), SQLITE_OK);
    HU_ASSERT_FALSE(hu_sqlite_quick_check_ok(raw));
    sqlite3_close(raw);
    HU_ASSERT_FALSE(hu_sqlite_open_sentinel_present(path));

    hu_memory_t mem = hu_sqlite_memory_create(&alloc, path);
    HU_ASSERT_NOT_NULL(mem.vtable); /* boots without the scan */
    HU_ASSERT_EQ(count_quarantine_files(path), 0);
    HU_ASSERT_TRUE(access(path, F_OK) == 0); /* original file untouched */

    mem.vtable->deinit(mem.ctx);
    remove_db_and_siblings(path);
}

/* Unclean-shutdown path: sentinel present -> full quick_check runs, the same
 * interior corruption is caught, and the file is quarantined + replaced. */
static void test_create_full_checks_and_quarantines_after_unclean_shutdown(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_sqlite_integrity_uncln_%d.db", (int)getpid());
    HU_ASSERT_TRUE(make_interior_corrupt_db(path));
    HU_ASSERT_TRUE(hu_sqlite_open_sentinel_write(path)); /* prior run died */

    hu_memory_t mem = hu_sqlite_memory_create(&alloc, path);
    HU_ASSERT_NOT_NULL(mem.vtable);
    HU_ASSERT_EQ(count_quarantine_files(path), 1); /* corrupt bytes preserved */

    /* Post: the db at `path` is a fresh valid replacement. */
    sqlite3 *raw = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &raw), SQLITE_OK);
    HU_ASSERT_TRUE(hu_sqlite_quick_check_ok(raw));
    sqlite3_close(raw);

    mem.vtable->deinit(mem.ctx);
    HU_ASSERT_FALSE(hu_sqlite_open_sentinel_present(path));
    remove_db_and_siblings(path);
}

/* ── Tri-state quick_check (2026-09-01) ──────────────────────────────────
 * On 08-04 the daemon quarantined a healthy 290 MB store: sqlite3_step() on
 * PRAGMA quick_check returned SQLITE_BUSY (a WAL checkpoint / another writer),
 * which the bool predicate read as "not ok" and treated as corruption. BUSY is
 * "I could not measure", not "the file is bad". These pin the distinction. */
static void test_quick_check_busy_is_indeterminate_not_corrupt(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_sqlite_integrity_busy_%d.db", (int)getpid());
    remove_db_and_siblings(path);
    sqlite3 *w = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &w), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_exec(w, "CREATE TABLE t(x); BEGIN EXCLUSIVE;", NULL, NULL, NULL),
                 SQLITE_OK);
    sqlite3 *r = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &r), SQLITE_OK); /* no busy timeout: BUSY at once */
    HU_ASSERT_EQ((int)hu_sqlite_quick_check(r), (int)HU_QC_INDETERMINATE);
    HU_ASSERT_FALSE(hu_sqlite_quick_check_ok(r)); /* bool wrapper: only OK is true */
    sqlite3_close(r);
    sqlite3_exec(w, "ROLLBACK;", NULL, NULL, NULL);
    sqlite3_close(w);
    remove_db_and_siblings(path);
}

static void test_quick_check_reports_corrupt_and_ok_distinctly(void) {
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_sqlite_integrity_tri_%d.db", (int)getpid());
    HU_ASSERT_TRUE(make_interior_corrupt_db(path));
    sqlite3 *raw = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &raw), SQLITE_OK);
    HU_ASSERT_EQ((int)hu_sqlite_quick_check(raw), (int)HU_QC_CORRUPT);
    sqlite3_close(raw);
    remove_db_and_siblings(path);
    HU_ASSERT_EQ(sqlite3_open(path, &raw), SQLITE_OK);
    sqlite3_exec(raw, "CREATE TABLE t(x); INSERT INTO t VALUES(1);", NULL, NULL, NULL);
    HU_ASSERT_EQ((int)hu_sqlite_quick_check(raw), (int)HU_QC_OK);
    sqlite3_close(raw);
    remove_db_and_siblings(path);
}

static void test_create_does_not_quarantine_busy_db_after_unclean_shutdown(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path[256];
    snprintf(path, sizeof(path), "/tmp/hu_sqlite_integrity_busyq_%d.db", (int)getpid());
    remove_db_and_siblings(path);
    {
        hu_memory_t m = hu_sqlite_memory_create(&alloc, path);
        HU_ASSERT_NOT_NULL(m.vtable);
        m.vtable->deinit(m.ctx);
    }
    HU_ASSERT_TRUE(hu_sqlite_open_sentinel_write(path)); /* "prior run died" */
    sqlite3 *w = NULL;
    HU_ASSERT_EQ(sqlite3_open(path, &w), SQLITE_OK);
    HU_ASSERT_EQ(sqlite3_exec(w, "BEGIN EXCLUSIVE;", NULL, NULL, NULL), SQLITE_OK);
    hu_memory_t m2 = hu_sqlite_memory_create(&alloc, path); /* must NOT quarantine */
    sqlite3_exec(w, "ROLLBACK;", NULL, NULL, NULL);
    sqlite3_close(w);
    HU_ASSERT_EQ(count_quarantine_files(path), 0);
    if (m2.vtable)
        m2.vtable->deinit(m2.ctx);
    remove_db_and_siblings(path);
}

void run_sqlite_integrity_tests(void) {
    HU_TEST_SUITE("sqlite_integrity");
    HU_RUN_TEST(test_quick_check_ok_on_valid_db);
    HU_RUN_TEST(test_quick_check_false_on_null);
    HU_RUN_TEST(test_quarantine_moves_corrupt_file_aside);
    HU_RUN_TEST(test_create_self_heals_corrupt_db);
    HU_RUN_TEST(test_open_sentinel_write_present_remove_roundtrip);
    HU_RUN_TEST(test_create_writes_sentinel_and_deinit_removes_it);
    HU_RUN_TEST(test_create_skips_full_check_after_clean_shutdown);
    HU_RUN_TEST(test_create_full_checks_and_quarantines_after_unclean_shutdown);
    HU_RUN_TEST(test_quick_check_busy_is_indeterminate_not_corrupt);
    HU_RUN_TEST(test_quick_check_reports_corrupt_and_ok_distinctly);
    HU_RUN_TEST(test_create_does_not_quarantine_busy_db_after_unclean_shutdown);
}

#else
void run_sqlite_integrity_tests(void) {
    (void)0;
}
#endif
