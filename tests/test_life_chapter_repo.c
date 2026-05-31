/* Exercises the sqlite-backed hu_life_chapter_repo_t implementation in
 * src/memory/repos/life_chapter_repo_sqlite.c (via hu_life_chapter_repo_create).
 * The filename reference above satisfies scripts/check-untested.sh, whose
 * basename matcher can't otherwise tie this test to that impl file (the exported
 * symbol is named for the abstraction, not the _sqlite impl).
 * DDD Phase 3 (one aggregate of ~22). */
#ifdef HU_ENABLE_SQLITE
#include "human/core/string.h"
#include "human/memory/engines.h"
#include "human/memory/life_chapter_repo.h"
#include "test_framework.h"
#include <string.h>

static void free_row(hu_allocator_t *a, hu_life_chapter_row_t *r) {
    if (r->theme)
        hu_str_free(a, r->theme);
    if (r->mood)
        hu_str_free(a, r->mood);
    if (r->key_threads_json)
        hu_str_free(a, r->key_threads_json);
    memset(r, 0, sizeof(*r));
}

static void test_life_chapter_repo_store_and_get_active(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_life_chapter_repo_t repo;
    HU_ASSERT_EQ(hu_life_chapter_repo_create(&mem, &alloc, &repo), HU_OK);

    /* nothing active yet */
    bool found = true;
    hu_life_chapter_row_t row = {0};
    HU_ASSERT_EQ(repo.vtable->get_active(repo.ctx, &alloc, &found, &row), HU_OK);
    HU_ASSERT_TRUE(!found);
    free_row(&alloc, &row);

    HU_ASSERT_EQ(repo.vtable->store_active(repo.ctx, "new job", "anxious", 1000, "[\"work\"]"),
                 HU_OK);

    HU_ASSERT_EQ(repo.vtable->get_active(repo.ctx, &alloc, &found, &row), HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_NOT_NULL(row.theme);
    HU_ASSERT_TRUE(strcmp(row.theme, "new job") == 0);
    HU_ASSERT_TRUE(row.mood && strcmp(row.mood, "anxious") == 0);
    HU_ASSERT_EQ(row.started_at, 1000);
    HU_ASSERT_TRUE(row.key_threads_json && strcmp(row.key_threads_json, "[\"work\"]") == 0);
    free_row(&alloc, &row);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

static void test_life_chapter_repo_store_deactivates_previous(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_life_chapter_repo_t repo;
    HU_ASSERT_EQ(hu_life_chapter_repo_create(&mem, &alloc, &repo), HU_OK);

    HU_ASSERT_EQ(repo.vtable->store_active(repo.ctx, "chapter A", "calm", 10, "[]"), HU_OK);
    HU_ASSERT_EQ(repo.vtable->store_active(repo.ctx, "chapter B", "excited", 20, "[]"), HU_OK);

    /* only the most recent chapter is active */
    bool found = false;
    hu_life_chapter_row_t row = {0};
    HU_ASSERT_EQ(repo.vtable->get_active(repo.ctx, &alloc, &found, &row), HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_TRUE(row.theme && strcmp(row.theme, "chapter B") == 0);
    HU_ASSERT_EQ(row.started_at, 20);
    free_row(&alloc, &row);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

static void test_life_chapter_repo_empty_strings_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
    hu_life_chapter_repo_t repo;
    HU_ASSERT_EQ(hu_life_chapter_repo_create(&mem, &alloc, &repo), HU_OK);

    /* empty theme/mood store; round-trips with NULL columns (dup_col returns
     * NULL for empty), started_at preserved. */
    HU_ASSERT_EQ(repo.vtable->store_active(repo.ctx, "", "", 42, "[]"), HU_OK);
    bool found = false;
    hu_life_chapter_row_t row = {0};
    HU_ASSERT_EQ(repo.vtable->get_active(repo.ctx, &alloc, &found, &row), HU_OK);
    HU_ASSERT_TRUE(found);
    HU_ASSERT_EQ(row.started_at, 42);
    free_row(&alloc, &row);

    repo.vtable->deinit(repo.ctx);
    mem.vtable->deinit(mem.ctx);
}

void run_life_chapter_repo_tests(void) {
    HU_TEST_SUITE("life_chapter_repo");
    HU_RUN_TEST(test_life_chapter_repo_store_and_get_active);
    HU_RUN_TEST(test_life_chapter_repo_store_deactivates_previous);
    HU_RUN_TEST(test_life_chapter_repo_empty_strings_ok);
}
#else
void run_life_chapter_repo_tests(void) {
    (void)0;
} /* gate stub, per test-source-gate-symmetry */
#endif
