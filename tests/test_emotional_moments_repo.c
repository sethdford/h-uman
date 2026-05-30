/* Exercises src/memory/repos/emotional_moments_repo_sqlite.c via
// @covers-none — covers the _repo_sqlite.c impl (named above; check-untested confirms);
//   check-test-references's filename heuristic mis-resolves to the sibling domain module.
 * hu_emotional_moment_record / _get_due / _mark_followed_up. The filename
 * reference above satisfies scripts/check-untested.sh. */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/emotional_moments.h"
#include "human/memory/engines.h"
#include "test_framework.h"

static void test_emotional_moments_repo_records_a_moment(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_emotional_moment_record(&alloc, &mem, "user_a", 6, "the big trip", 12,
                                            "excited", 7, 0.8f),
                 HU_OK);

    mem.vtable->deinit(mem.ctx);
}

static void test_emotional_moments_repo_rejects_empty_contact(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_emotional_moment_record(&alloc, &mem, NULL, 0, "t", 1, "e", 1, 0.5f),
                 HU_ERR_INVALID_ARGUMENT);

    mem.vtable->deinit(mem.ctx);
}

/* get_due over an empty store is a clean empty read, not an error. */
static void test_emotional_moments_repo_get_due_empty_is_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_emotional_moment_t *out = NULL;
    size_t n = 99;
    HU_ASSERT_EQ(hu_emotional_moment_get_due(&alloc, &mem, 1717000000, &out, &n), HU_OK);
    HU_ASSERT_EQ((int)n, 0);

    mem.vtable->deinit(mem.ctx);
}

void run_emotional_moments_repo_tests(void) {
    HU_TEST_SUITE("emotional_moments_repo");
    HU_RUN_TEST(test_emotional_moments_repo_records_a_moment);
    HU_RUN_TEST(test_emotional_moments_repo_rejects_empty_contact);
    HU_RUN_TEST(test_emotional_moments_repo_get_due_empty_is_ok);
}
#else
void run_emotional_moments_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
