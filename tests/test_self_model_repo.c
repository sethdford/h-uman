/* Exercises src/memory/repos/self_model_repo_sqlite.c via
// @covers-none — covers the _repo_sqlite.c impl (named above; check-untested confirms);
//   check-test-references's filename heuristic mis-resolves to the sibling domain module.
 * hu_agent_self_model_init_tables / _compute_and_insert_observation. The
 * filename reference above satisfies scripts/check-untested.sh. */
/* Gate matches src/memory/repos/self_model_repo_sqlite.c, whose functions are
 * compiled only under HU_ENABLE_SELF_MODEL && HU_ENABLE_SQLITE (per
 * test-source-gate-symmetry). */
#if defined(HU_ENABLE_SELF_MODEL) && defined(HU_ENABLE_SQLITE)
#include "human/agent/self_model.h"
#include "human/memory/engines.h"
#include "test_framework.h"

static void test_self_model_repo_init_tables_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    HU_ASSERT_EQ(hu_agent_self_model_init_tables(db), HU_OK);
    /* Idempotent — CREATE TABLE IF NOT EXISTS, safe to call twice. */
    HU_ASSERT_EQ(hu_agent_self_model_init_tables(db), HU_OK);

    mem.vtable->deinit(mem.ctx);
}

/* compute_and_insert rejects a NULL behavior log (the guard) rather than
 * dereferencing it. */
static void test_self_model_repo_compute_rejects_null_log(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_EQ(hu_agent_self_model_init_tables(db), HU_OK);

    HU_ASSERT_EQ(hu_agent_self_model_compute_and_insert_observation(db, NULL, 0, 1717000000000LL,
                                                                    NULL, NULL),
                 HU_ERR_INVALID_ARGUMENT);

    mem.vtable->deinit(mem.ctx);
}

void run_self_model_repo_tests(void) {
    HU_TEST_SUITE("self_model_repo");
    HU_RUN_TEST(test_self_model_repo_init_tables_ok);
    HU_RUN_TEST(test_self_model_repo_compute_rejects_null_log);
}
#else
void run_self_model_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
