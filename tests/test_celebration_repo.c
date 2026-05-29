/* tests/test_celebration_repo.c — pins B1c anti-re-celebration repository.
 * Spec: docs/plans/2026-05-29-prosocial-uplift/ */

typedef int hu_test_celebration_repo_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/celebration_repo.h"
#include "test_framework.h"
#include <string.h>

static void make_celebration(hu_celebration_t *c, const char *cid, const char *wk, int64_t ts) {
    memset(c, 0, sizeof(*c));
    c->contact_id = cid;
    c->contact_id_len = strlen(cid);
    c->win_key = wk;
    c->win_key_len = strlen(wk);
    c->kind = 1;
    c->celebrated_at = ts;
}

static void celebration_repo_record_then_recent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_celebration_repo_t repo;
    HU_ASSERT_EQ(hu_celebration_repo_create(&mem, &alloc, &repo), HU_OK);

    /* Not celebrated yet. */
    bool recent = true;
    HU_ASSERT_EQ(
        repo.vtable->was_recent(repo.ctx, "seth", 4, "passed-bar", 10, 1000, 3600, &recent), HU_OK);
    HU_ASSERT_FALSE(recent);

    /* Record it. */
    hu_celebration_t c;
    make_celebration(&c, "seth", "passed-bar", 1000);
    HU_ASSERT_EQ(repo.vtable->record(repo.ctx, &c), HU_OK);

    /* Now it IS recent within the window. */
    HU_ASSERT_EQ(
        repo.vtable->was_recent(repo.ctx, "seth", 4, "passed-bar", 10, 1500, 3600, &recent), HU_OK);
    HU_ASSERT_TRUE(recent);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

static void celebration_repo_outside_window_is_not_recent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_celebration_repo_t repo;
    HU_ASSERT_EQ(hu_celebration_repo_create(&mem, &alloc, &repo), HU_OK);

    hu_celebration_t c;
    make_celebration(&c, "seth", "ten-years", 1000);
    HU_ASSERT_EQ(repo.vtable->record(repo.ctx, &c), HU_OK);

    /* now=100000, window=3600 -> celebrated_at(1000) is far outside. */
    bool recent = true;
    HU_ASSERT_EQ(
        repo.vtable->was_recent(repo.ctx, "seth", 4, "ten-years", 9, 100000, 3600, &recent), HU_OK);
    HU_ASSERT_FALSE(recent);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

static void celebration_repo_distinct_keys_independent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_celebration_repo_t repo;
    HU_ASSERT_EQ(hu_celebration_repo_create(&mem, &alloc, &repo), HU_OK);

    hu_celebration_t c;
    make_celebration(&c, "seth", "win-a", 1000);
    HU_ASSERT_EQ(repo.vtable->record(repo.ctx, &c), HU_OK);

    /* A different win for the same contact is independent. */
    bool recent = true;
    HU_ASSERT_EQ(repo.vtable->was_recent(repo.ctx, "seth", 4, "win-b", 5, 1500, 3600, &recent),
                 HU_OK);
    HU_ASSERT_FALSE(recent);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

void run_celebration_repo_tests(void);
void run_celebration_repo_tests(void) {
    HU_TEST_SUITE("celebration_repo");
    HU_RUN_TEST(celebration_repo_record_then_recent);
    HU_RUN_TEST(celebration_repo_outside_window_is_not_recent);
    HU_RUN_TEST(celebration_repo_distinct_keys_independent);
}

#else
void run_celebration_repo_tests(void) {
    (void)0;
}
#endif /* HU_ENABLE_SQLITE */
