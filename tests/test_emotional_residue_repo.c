/* Exercises src/memory/repos/emotional_residue_repo_sqlite.c via
// @covers-none — covers the _repo_sqlite.c impl (named above; check-untested confirms);
//   check-test-references's filename heuristic mis-resolves to the sibling domain module.
 * hu_emotional_residue_add. The filename reference above satisfies
 * scripts/check-untested.sh. */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/emotional_residue.h"
#include "human/memory/engines.h"
#include "test_framework.h"
#include <sqlite3.h>

static int residue_count(sqlite3 *db, const char *cid) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM emotional_residue WHERE contact_id=?1;", -1,
                           &st, NULL) != SQLITE_OK)
        return -1;
    sqlite3_bind_text(st, 1, cid, -1, SQLITE_STATIC);
    int n = (sqlite3_step(st) == SQLITE_ROW) ? sqlite3_column_int(st, 0) : -1;
    sqlite3_finalize(st);
    return n;
}

static void test_emotional_residue_repo_add_persists(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t id = 0;
    HU_ASSERT_EQ(hu_emotional_residue_add(db, 0, "user_a", 6, 0.5, 0.8, 0.1, &id), HU_OK);
    HU_ASSERT_TRUE(id > 0);
    HU_ASSERT_EQ(residue_count(db, "user_a"), 1);

    mem.vtable->deinit(mem.ctx);
}

static void test_emotional_residue_repo_rejects_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);

    HU_ASSERT_EQ(hu_emotional_residue_add(db, 0, "user_a", 6, 0.5, 0.8, 0.1, NULL),
                 HU_ERR_INVALID_ARGUMENT);

    mem.vtable->deinit(mem.ctx);
}

void run_emotional_residue_repo_tests(void) {
    HU_TEST_SUITE("emotional_residue_repo");
    HU_RUN_TEST(test_emotional_residue_repo_add_persists);
    HU_RUN_TEST(test_emotional_residue_repo_rejects_null_out);
}
#else
void run_emotional_residue_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
