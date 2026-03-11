#ifdef HU_ENABLE_SQLITE

#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "human/memory/life_chapters.h"
#include "human/persona.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

static void test_life_chapter_store_get_active_returns_it(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_life_chapter_t chapter = {0};
    snprintf(chapter.theme, sizeof(chapter.theme), "career transition");
    snprintf(chapter.mood, sizeof(chapter.mood), "focused");
    chapter.started_at = (int64_t)time(NULL);
    snprintf(chapter.key_threads[0], sizeof(chapter.key_threads[0]), "new job search");
    chapter.key_threads_count = 1;

    hu_error_t err = hu_life_chapter_store(&alloc, &mem, &chapter);
    HU_ASSERT_EQ(err, HU_OK);

    hu_life_chapter_t out = {0};
    err = hu_life_chapter_get_active(&alloc, &mem, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_STR_EQ(out.theme, "career transition");
    HU_ASSERT_STR_EQ(out.mood, "focused");
    HU_ASSERT_STR_EQ(out.key_threads[0], "new job search");
    HU_ASSERT_EQ(out.key_threads_count, 1u);

    mem.vtable->deinit(mem.ctx);
}

static void test_life_chapter_build_directive_contains_theme_and_mood(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_life_chapter_t chapter = {0};
    snprintf(chapter.theme, sizeof(chapter.theme), "creative phase");
    snprintf(chapter.mood, sizeof(chapter.mood), "inspired");
    chapter.started_at = 1000;
    snprintf(chapter.key_threads[0], sizeof(chapter.key_threads[0]), "writing");
    chapter.key_threads_count = 1;

    size_t len = 0;
    char *dir = hu_life_chapter_build_directive(&alloc, &chapter, &len);
    HU_ASSERT_NOT_NULL(dir);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(dir, "creative phase") != NULL);
    HU_ASSERT_TRUE(strstr(dir, "inspired") != NULL);
    HU_ASSERT_TRUE(strstr(dir, "writing") != NULL);
    HU_ASSERT_TRUE(strstr(dir, "LIFE CHAPTER") != NULL);

    hu_str_free(&alloc, dir);
}

static void test_life_chapter_no_active_returns_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_life_chapter_t out = {0};
    hu_error_t err = hu_life_chapter_get_active(&alloc, &mem, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.theme[0], '\0');
    HU_ASSERT_EQ(out.mood[0], '\0');
    HU_ASSERT_EQ(out.key_threads_count, 0u);

    mem.vtable->deinit(mem.ctx);
}

static void test_life_chapter_build_directive_empty_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_life_chapter_t chapter = {0};

    size_t len = 0;
    char *dir = hu_life_chapter_build_directive(&alloc, &chapter, &len);
    HU_ASSERT_NULL(dir);
    HU_ASSERT_EQ(len, 0u);
}

void run_life_chapters_tests(void) {
    HU_TEST_SUITE("life_chapters");
    HU_RUN_TEST(test_life_chapter_store_get_active_returns_it);
    HU_RUN_TEST(test_life_chapter_build_directive_contains_theme_and_mood);
    HU_RUN_TEST(test_life_chapter_no_active_returns_empty);
    HU_RUN_TEST(test_life_chapter_build_directive_empty_returns_null);
}

#else

void run_life_chapters_tests(void) {
    (void)0;
}

#endif /* HU_ENABLE_SQLITE */
