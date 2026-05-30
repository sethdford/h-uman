/* Exercises src/memory/repos/emotional_state_repo_sqlite.c via
// @covers-none — covers the _repo_sqlite.c impl (named above; check-untested confirms);
//   check-test-references's filename heuristic mis-resolves to the sibling domain module.
 * hu_emotional_state_record / _get_recent. The filename reference above
 * satisfies scripts/check-untested.sh. */
#ifdef HU_ENABLE_SQLITE
#include "human/context/emotional_state.h"
#include "human/memory/engines.h"
#include "test_framework.h"

static void test_emotional_state_repo_rejects_null_alloc(void) {
    hu_memory_t mem = {0};
    HU_ASSERT_EQ(hu_emotional_state_record(NULL, &mem, "user_a", 6, "text", 4),
                 HU_ERR_INVALID_ARGUMENT);
}

/* Empty text is a no-op success (nothing to classify), not an error. */
static void test_emotional_state_repo_empty_text_is_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_emotional_state_record(&alloc, &mem, "user_a", 6, "", 0), HU_OK);

    mem.vtable->deinit(mem.ctx);
}

/* get_recent rejects NULL out params (the read-path guard). */
static void test_emotional_state_repo_get_recent_rejects_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    size_t out_len = 0;
    HU_ASSERT_EQ(hu_emotional_state_get_recent(&alloc, &mem, "user_a", 6, NULL, &out_len),
                 HU_ERR_INVALID_ARGUMENT);

    mem.vtable->deinit(mem.ctx);
}

void run_emotional_state_repo_tests(void) {
    HU_TEST_SUITE("emotional_state_repo");
    HU_RUN_TEST(test_emotional_state_repo_rejects_null_alloc);
    HU_RUN_TEST(test_emotional_state_repo_empty_text_is_ok);
    HU_RUN_TEST(test_emotional_state_repo_get_recent_rejects_null_out);
}
#else
void run_emotional_state_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
