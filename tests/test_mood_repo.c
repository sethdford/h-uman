/* Exercises src/memory/repos/mood_repo_sqlite.c via hu_mood_set /
// @covers-none — covers the _repo_sqlite.c impl (named above; check-untested confirms);
//   check-test-references's filename heuristic mis-resolves to the sibling domain module.
 * hu_mood_get_current. The filename reference above satisfies
 * scripts/check-untested.sh. */
#ifdef HU_ENABLE_SQLITE
#include "human/memory/engines.h"
#include "human/persona/mood.h"
#include "test_framework.h"

/* Round-trip: set a mood, read it back. set_at is "now", so the exponential
 * decay applied by get_current is negligible and the strong intensity stays
 * well above the 0.1 neutral-collapse floor. */
static void test_mood_repo_set_then_get(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_mood_set(&alloc, &mem, HU_MOOD_HAPPY, 0.9f, "good news", 9), HU_OK);

    hu_mood_state_t st;
    HU_ASSERT_EQ(hu_mood_get_current(&alloc, &mem, &st), HU_OK);
    HU_ASSERT_EQ((int)st.mood, (int)HU_MOOD_HAPPY);
    HU_ASSERT_TRUE(st.intensity > 0.5f);

    mem.vtable->deinit(mem.ctx);
}

/* Empty store reads back as neutral, not an error. */
static void test_mood_repo_empty_is_neutral(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_mood_state_t st;
    HU_ASSERT_EQ(hu_mood_get_current(&alloc, &mem, &st), HU_OK);
    HU_ASSERT_EQ((int)st.mood, (int)HU_MOOD_NEUTRAL);

    mem.vtable->deinit(mem.ctx);
}

static void test_mood_repo_get_rejects_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    HU_ASSERT_EQ(hu_mood_get_current(&alloc, &mem, NULL), HU_ERR_INVALID_ARGUMENT);

    mem.vtable->deinit(mem.ctx);
}

void run_mood_repo_tests(void) {
    HU_TEST_SUITE("mood_repo");
    HU_RUN_TEST(test_mood_repo_set_then_get);
    HU_RUN_TEST(test_mood_repo_empty_is_neutral);
    HU_RUN_TEST(test_mood_repo_get_rejects_null_out);
}
#else
void run_mood_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
