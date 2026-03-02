#include "test_framework.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/arena.h"
#include <string.h>

static void test_system_allocator_basic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    void *p = alloc.alloc(alloc.ctx, 128);
    SC_ASSERT_NOT_NULL(p);
    memset(p, 0xAB, 128);
    alloc.free(alloc.ctx, p, 128);
}

static void test_system_allocator_realloc(void) {
    sc_allocator_t alloc = sc_system_allocator();
    void *p = alloc.alloc(alloc.ctx, 64);
    SC_ASSERT_NOT_NULL(p);
    memset(p, 0x42, 64);
    p = alloc.realloc(alloc.ctx, p, 64, 256);
    SC_ASSERT_NOT_NULL(p);
    unsigned char *bytes = (unsigned char *)p;
    for (int i = 0; i < 64; i++) SC_ASSERT_EQ(bytes[i], 0x42);
    alloc.free(alloc.ctx, p, 256);
}

static void test_tracking_allocator_no_leaks(void) {
    sc_tracking_allocator_t *ta = sc_tracking_allocator_create();
    sc_allocator_t alloc = sc_tracking_allocator_allocator(ta);

    void *a = alloc.alloc(alloc.ctx, 100);
    void *b = alloc.alloc(alloc.ctx, 200);
    SC_ASSERT_NOT_NULL(a);
    SC_ASSERT_NOT_NULL(b);
    SC_ASSERT_EQ(sc_tracking_allocator_leaks(ta), 2);

    alloc.free(alloc.ctx, a, 100);
    alloc.free(alloc.ctx, b, 200);
    SC_ASSERT_EQ(sc_tracking_allocator_leaks(ta), 0);
    SC_ASSERT_EQ(sc_tracking_allocator_total_allocated(ta), 300);
    SC_ASSERT_EQ(sc_tracking_allocator_total_freed(ta), 300);

    sc_tracking_allocator_destroy(ta);
}

static void test_tracking_allocator_detects_leaks(void) {
    sc_tracking_allocator_t *ta = sc_tracking_allocator_create();
    sc_allocator_t alloc = sc_tracking_allocator_allocator(ta);

    alloc.alloc(alloc.ctx, 64);
    alloc.alloc(alloc.ctx, 128);

    SC_ASSERT_EQ(sc_tracking_allocator_leaks(ta), 2);
    SC_ASSERT_EQ(sc_tracking_allocator_total_allocated(ta), 192);
    SC_ASSERT_EQ(sc_tracking_allocator_total_freed(ta), 0);

    sc_tracking_allocator_destroy(ta);
}

static void test_arena_basic(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_arena_t *arena = sc_arena_create(sys);
    SC_ASSERT_NOT_NULL(arena);

    sc_allocator_t alloc = sc_arena_allocator(arena);
    char *s1 = (char *)alloc.alloc(alloc.ctx, 32);
    SC_ASSERT_NOT_NULL(s1);
    strcpy(s1, "hello");

    char *s2 = (char *)alloc.alloc(alloc.ctx, 64);
    SC_ASSERT_NOT_NULL(s2);
    strcpy(s2, "world");

    SC_ASSERT_STR_EQ(s1, "hello");
    SC_ASSERT_STR_EQ(s2, "world");
    SC_ASSERT(sc_arena_bytes_used(arena) == 96);

    sc_arena_reset(arena);
    SC_ASSERT_EQ(sc_arena_bytes_used(arena), 0);

    sc_arena_destroy(arena);
}

static void test_arena_large_alloc(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_arena_t *arena = sc_arena_create(sys);
    sc_allocator_t alloc = sc_arena_allocator(arena);

    void *big = alloc.alloc(alloc.ctx, 8192);
    SC_ASSERT_NOT_NULL(big);
    memset(big, 0xFF, 8192);

    sc_arena_destroy(arena);
}

void run_allocator_tests(void) {
    SC_TEST_SUITE("allocator");
    SC_RUN_TEST(test_system_allocator_basic);
    SC_RUN_TEST(test_system_allocator_realloc);
    SC_RUN_TEST(test_tracking_allocator_no_leaks);
    SC_RUN_TEST(test_tracking_allocator_detects_leaks);
    SC_RUN_TEST(test_arena_basic);
    SC_RUN_TEST(test_arena_large_alloc);
}
