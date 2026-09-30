/* tests/test_preferences.c
 *
 * hu_preferences_load (src/agent/preferences.c) must free every field of
 * every recalled entry. The SQLite engine allocates a custom category name
 * per entry (read_entry_from_row); a hand-written free loop that skipped it
 * leaked a few bytes on every agent turn. LeakSanitizer only runs on the
 * Linux CI job, so this test counts allocations directly and fails on macOS
 * too.
 */
#include "test_framework.h"

#ifdef HU_ENABLE_SQLITE

#include "human/agent/preferences.h"
#include "human/core/allocator.h"
#include "human/memory.h"
#include <string.h>

static void preferences_load_frees_every_recalled_field(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&sys, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);

    const char *p1 = "prefers short replies";
    const char *p2 = "no emojis please";
    HU_ASSERT_EQ(hu_preferences_store(&mem, &sys, p1, strlen(p1)), HU_OK);
    HU_ASSERT_EQ(hu_preferences_store(&mem, &sys, p2, strlen(p2)), HU_OK);

    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);
    char *out = NULL;
    size_t out_len = 0;
    HU_ASSERT_EQ(hu_preferences_load(&mem, &alloc, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_NOT_NULL(strstr(out, p1));
    HU_ASSERT_NOT_NULL(strstr(out, p2));
    alloc.free(alloc.ctx, out, out_len + 1);

    size_t leaks = hu_tracking_allocator_leaks(ta);
    hu_tracking_allocator_destroy(ta);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
    HU_ASSERT_EQ(leaks, (size_t)0);
}

void run_preferences_tests(void) {
    HU_TEST_SUITE("preferences");
    HU_RUN_TEST(preferences_load_frees_every_recalled_field);
}

#else

void run_preferences_tests(void) {
    (void)0;
}

#endif
