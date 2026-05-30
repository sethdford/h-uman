/* Exercises src/memory/repos/theory_of_mind_repo_sqlite.c via
// @covers-none — covers the _repo_sqlite.c impl (named above; check-untested confirms);
//   check-test-references's filename heuristic mis-resolves to the sibling domain module.
 * hu_tom_user_expectations_init_table / _persist_user_expectation /
 * _count_for_contact. The filename reference above satisfies
 * scripts/check-untested.sh. */
#ifdef HU_ENABLE_SQLITE
#include "human/agent/theory_of_mind.h"
#include "human/memory/engines.h"
#include "test_framework.h"

static void test_theory_of_mind_repo_persist_then_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    HU_ASSERT_EQ(hu_tom_user_expectations_init_table(db), HU_OK);
    HU_ASSERT_EQ(hu_tom_persist_user_expectation(db, "user_a", "the project", 11,
                                                 HU_TOM_EXPECT_REMEMBERS, "sess1", 5, 1,
                                                 1717000000000LL),
                 HU_OK);

    int64_t n = 0;
    HU_ASSERT_EQ(hu_tom_user_expectations_count_for_contact(db, "user_a", &n), HU_OK);
    HU_ASSERT_EQ((int)n, 1);

    mem.vtable->deinit(mem.ctx);
}

static void test_theory_of_mind_repo_count_rejects_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_tom_user_expectations_init_table(db), HU_OK);

    HU_ASSERT_EQ(hu_tom_user_expectations_count_for_contact(db, "user_a", NULL),
                 HU_ERR_INVALID_ARGUMENT);

    mem.vtable->deinit(mem.ctx);
}

void run_theory_of_mind_repo_tests(void) {
    HU_TEST_SUITE("theory_of_mind_repo");
    HU_RUN_TEST(test_theory_of_mind_repo_persist_then_count);
    HU_RUN_TEST(test_theory_of_mind_repo_count_rejects_null_out);
}
#else
void run_theory_of_mind_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
