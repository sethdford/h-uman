#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "test_framework.h"
#include <string.h>

static void test_system_allocator_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    void *p = alloc.alloc(alloc.ctx, 128);
    HU_ASSERT_NOT_NULL(p);
    memset(p, 0xAB, 128);
    alloc.free(alloc.ctx, p, 128);
}

static void test_system_allocator_realloc(void) {
    hu_allocator_t alloc = hu_system_allocator();
    void *p = alloc.alloc(alloc.ctx, 64);
    HU_ASSERT_NOT_NULL(p);
    memset(p, 0x42, 64);
    p = alloc.realloc(alloc.ctx, p, 64, 256);
    HU_ASSERT_NOT_NULL(p);
    unsigned char *bytes = (unsigned char *)p;
    for (int i = 0; i < 64; i++)
        HU_ASSERT_EQ(bytes[i], 0x42);
    alloc.free(alloc.ctx, p, 256);
}

static void test_tracking_allocator_no_leaks(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    void *a = alloc.alloc(alloc.ctx, 100);
    void *b = alloc.alloc(alloc.ctx, 200);
    HU_ASSERT_NOT_NULL(a);
    HU_ASSERT_NOT_NULL(b);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 2);

    alloc.free(alloc.ctx, a, 100);
    alloc.free(alloc.ctx, b, 200);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0);
    HU_ASSERT_EQ(hu_tracking_allocator_total_allocated(ta), 300);
    HU_ASSERT_EQ(hu_tracking_allocator_total_freed(ta), 300);

    hu_tracking_allocator_destroy(ta);
}

static void test_tracking_allocator_detects_leaks(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    alloc.alloc(alloc.ctx, 64);
    alloc.alloc(alloc.ctx, 128);

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 2);
    HU_ASSERT_EQ(hu_tracking_allocator_total_allocated(ta), 192);
    HU_ASSERT_EQ(hu_tracking_allocator_total_freed(ta), 0);

    hu_tracking_allocator_destroy(ta);
}

static void test_arena_basic(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(sys);
    HU_ASSERT_NOT_NULL(arena);

    hu_allocator_t alloc = hu_arena_allocator(arena);
    char *s1 = (char *)alloc.alloc(alloc.ctx, 32);
    HU_ASSERT_NOT_NULL(s1);
    strcpy(s1, "hello");

    char *s2 = (char *)alloc.alloc(alloc.ctx, 64);
    HU_ASSERT_NOT_NULL(s2);
    strcpy(s2, "world");

    HU_ASSERT_STR_EQ(s1, "hello");
    HU_ASSERT_STR_EQ(s2, "world");
    HU_ASSERT(hu_arena_bytes_used(arena) == 96);

    hu_arena_reset(arena);
    HU_ASSERT_EQ(hu_arena_bytes_used(arena), 0);

    hu_arena_destroy(arena);
}

static void test_arena_large_alloc(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(sys);
    hu_allocator_t alloc = hu_arena_allocator(arena);

    void *big = alloc.alloc(alloc.ctx, 8192);
    HU_ASSERT_NOT_NULL(big);
    memset(big, 0xFF, 8192);

    hu_arena_destroy(arena);
}

void run_allocator_tests(void) {
    HU_TEST_SUITE("allocator");
    HU_RUN_TEST(test_system_allocator_basic);
    HU_RUN_TEST(test_system_allocator_realloc);
    HU_RUN_TEST(test_tracking_allocator_no_leaks);
    HU_RUN_TEST(test_tracking_allocator_detects_leaks);
    HU_RUN_TEST(test_arena_basic);
    HU_RUN_TEST(test_arena_large_alloc);
}
