/* Core utility edge cases (~30 tests). */
#include "human/core/allocator.h"
#include "human/core/arena.h"
#include "human/core/error.h"
#include "human/core/slice.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void test_arena_multiple_allocs(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(sys);
    hu_allocator_t alloc = hu_arena_allocator(arena);
    void *p1 = alloc.alloc(alloc.ctx, 16);
    void *p2 = alloc.alloc(alloc.ctx, 32);
    void *p3 = alloc.alloc(alloc.ctx, 64);
    HU_ASSERT_NOT_NULL(p1);
    HU_ASSERT_NOT_NULL(p2);
    HU_ASSERT_NOT_NULL(p3);
    HU_ASSERT(p1 != p2);
    HU_ASSERT(p2 != p3);
    HU_ASSERT_EQ(hu_arena_bytes_used(arena), 112u);
    hu_arena_destroy(arena);
}

static void test_arena_reset(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(sys);
    hu_allocator_t alloc = hu_arena_allocator(arena);
    alloc.alloc(alloc.ctx, 100);
    alloc.alloc(alloc.ctx, 200);
    HU_ASSERT_EQ(hu_arena_bytes_used(arena), 300u);
    hu_arena_reset(arena);
    HU_ASSERT_EQ(hu_arena_bytes_used(arena), 0u);
    hu_arena_destroy(arena);
}

static void test_string_empty_concat(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_str_concat(&alloc, HU_STR_LIT(""), HU_STR_LIT(""));
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_EQ(strlen(s), 0u);
    hu_str_free(&alloc, s);
}

static void test_string_null_inputs(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_strdup(&alloc, NULL);
    HU_ASSERT_NULL(s);
}

static void test_string_very_long(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[10000];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'x';
    buf[sizeof(buf) - 1] = '\0';
    char *s = hu_strdup(&alloc, buf);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_EQ(strlen(s), 9999u);
    hu_str_free(&alloc, s);
}

static void test_allocator_zero_size(void) {
    hu_allocator_t alloc = hu_system_allocator();
    void *p = alloc.alloc(alloc.ctx, 0);
    if (p)
        alloc.free(alloc.ctx, p, 0);
}

static void test_allocator_realloc_grow_shrink(void) {
    hu_allocator_t alloc = hu_system_allocator();
    void *p = alloc.alloc(alloc.ctx, 64);
    HU_ASSERT_NOT_NULL(p);
    memset(p, 0x42, 64);
    p = alloc.realloc(alloc.ctx, p, 64, 256);
    HU_ASSERT_NOT_NULL(p);
    unsigned char *b = (unsigned char *)p;
    for (int i = 0; i < 64; i++)
        HU_ASSERT_EQ(b[i], 0x42);
    p = alloc.realloc(alloc.ctx, p, 256, 32);
    HU_ASSERT_NOT_NULL(p);
    alloc.free(alloc.ctx, p, 32);
}

static void test_tracking_allocator_reports_leak_count(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);
    void *p = alloc.alloc(alloc.ctx, 100);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 1u);
    alloc.free(alloc.ctx, p, 100);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0u);
    hu_tracking_allocator_destroy(ta);
}

static void test_error_string_all_codes(void) {
    const char *s0 = hu_error_string(HU_OK);
    const char *s1 = hu_error_string(HU_ERR_OUT_OF_MEMORY);
    const char *s2 = hu_error_string(HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NOT_NULL(s0);
    HU_ASSERT_NOT_NULL(s1);
    HU_ASSERT_NOT_NULL(s2);
    HU_ASSERT(strlen(s0) > 0);
}

static void test_strndup_zero_len(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_strndup(&alloc, "hello", 0);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_EQ(strlen(s), 0u);
    hu_str_free(&alloc, s);
}

static void test_str_join_empty_parts(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_str_t parts[] = {HU_STR_LIT("")};
    char *s = hu_str_join(&alloc, parts, 1, HU_STR_LIT(", "));
    HU_ASSERT_NOT_NULL(s);
    hu_str_free(&alloc, s);
}

static void test_str_join_single(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_str_t parts[] = {HU_STR_LIT("a")};
    char *s = hu_str_join(&alloc, parts, 1, HU_STR_LIT("-"));
    HU_ASSERT_STR_EQ(s, "a");
    hu_str_free(&alloc, s);
}

static void test_str_contains_empty_needle(void) {
    HU_ASSERT_TRUE(hu_str_contains(HU_STR_LIT("hello"), HU_STR_LIT("")));
}

static void test_str_index_of(void) {
    int idx = hu_str_index_of(HU_STR_LIT("hello world"), HU_STR_LIT("world"));
    HU_ASSERT_EQ(idx, 6);
    idx = hu_str_index_of(HU_STR_LIT("hello"), HU_STR_LIT("xyz"));
    HU_ASSERT_EQ(idx, -1);
}

static void test_str_starts_with(void) {
    HU_ASSERT_TRUE(hu_str_starts_with(HU_STR_LIT("hello"), HU_STR_LIT("hell")));
    HU_ASSERT_FALSE(hu_str_starts_with(HU_STR_LIT("hello"), HU_STR_LIT("ell")));
}

static void test_str_ends_with(void) {
    HU_ASSERT_TRUE(hu_str_ends_with(HU_STR_LIT("hello"), HU_STR_LIT("llo")));
    HU_ASSERT_FALSE(hu_str_ends_with(HU_STR_LIT("hello"), HU_STR_LIT("ell")));
}

static void test_str_trim_all_space(void) {
    hu_str_t t = hu_str_trim(HU_STR_LIT("   "));
    HU_ASSERT_EQ(t.len, 0u);
}

static void test_str_is_empty(void) {
    HU_ASSERT_TRUE(hu_str_is_empty(HU_STR_NULL));
    HU_ASSERT_TRUE(hu_str_is_empty(HU_STR_LIT("")));
    HU_ASSERT_FALSE(hu_str_is_empty(HU_STR_LIT("x")));
}

static void test_sprintf_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_sprintf(&alloc, "%d %s", 42, "test");
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_TRUE(strstr(s, "42") != NULL);
    HU_ASSERT_TRUE(strstr(s, "test") != NULL);
    hu_str_free(&alloc, s);
}

static void test_arena_alloc_after_reset(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(sys);
    hu_allocator_t alloc = hu_arena_allocator(arena);
    alloc.alloc(alloc.ctx, 50);
    hu_arena_reset(arena);
    void *p = alloc.alloc(alloc.ctx, 25);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_EQ(hu_arena_bytes_used(arena), 25u);
    hu_arena_destroy(arena);
}

static void test_tracking_allocator_total(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);
    alloc.alloc(alloc.ctx, 10);
    alloc.alloc(alloc.ctx, 20);
    HU_ASSERT_EQ(hu_tracking_allocator_total_allocated(ta), 30u);
    hu_tracking_allocator_destroy(ta);
}

static void test_error_string_invalid(void) {
    const char *s = hu_error_string((hu_error_t)999);
    HU_ASSERT_NOT_NULL(s);
}

static void test_str_from_cstr(void) {
    hu_str_t s = hu_str_from_cstr("hello");
    HU_ASSERT_EQ(s.len, 5u);
    HU_ASSERT_STR_EQ(s.ptr, "hello");
}

static void test_str_eq_cstr(void) {
    HU_ASSERT_TRUE(hu_str_eq_cstr(HU_STR_LIT("abc"), "abc"));
    HU_ASSERT_FALSE(hu_str_eq_cstr(HU_STR_LIT("abc"), "abd"));
}

static void test_bytes_from(void) {
    uint8_t data[] = {1, 2, 3};
    hu_bytes_t b = hu_bytes_from(data, 3);
    HU_ASSERT_EQ(b.len, 3u);
    HU_ASSERT_EQ(b.ptr[0], 1);
}

static void test_str_concat_normal(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_str_concat(&alloc, HU_STR_LIT("foo"), HU_STR_LIT("bar"));
    HU_ASSERT_STR_EQ(s, "foobar");
    hu_str_free(&alloc, s);
}

static void test_str_join_multiple(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_str_t parts[] = {HU_STR_LIT("a"), HU_STR_LIT("b"), HU_STR_LIT("c")};
    char *s = hu_str_join(&alloc, parts, 3, HU_STR_LIT("|"));
    HU_ASSERT_STR_EQ(s, "a|b|c");
    hu_str_free(&alloc, s);
}

static void test_arena_large_block(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_arena_t *arena = hu_arena_create(sys);
    hu_allocator_t alloc = hu_arena_allocator(arena);
    void *p = alloc.alloc(alloc.ctx, 65536);
    HU_ASSERT_NOT_NULL(p);
    hu_arena_destroy(arena);
}

void run_core_extended_tests(void) {
    HU_TEST_SUITE("Core Extended");
    HU_RUN_TEST(test_arena_multiple_allocs);
    HU_RUN_TEST(test_arena_reset);
    HU_RUN_TEST(test_string_empty_concat);
    HU_RUN_TEST(test_string_null_inputs);
    HU_RUN_TEST(test_string_very_long);
    HU_RUN_TEST(test_allocator_zero_size);
    HU_RUN_TEST(test_allocator_realloc_grow_shrink);
    HU_RUN_TEST(test_tracking_allocator_reports_leak_count);
    HU_RUN_TEST(test_error_string_all_codes);
    HU_RUN_TEST(test_strndup_zero_len);
    HU_RUN_TEST(test_str_join_empty_parts);
    HU_RUN_TEST(test_str_join_single);
    HU_RUN_TEST(test_str_contains_empty_needle);
    HU_RUN_TEST(test_str_index_of);
    HU_RUN_TEST(test_str_starts_with);
    HU_RUN_TEST(test_str_ends_with);
    HU_RUN_TEST(test_str_trim_all_space);
    HU_RUN_TEST(test_str_is_empty);
    HU_RUN_TEST(test_sprintf_format);
    HU_RUN_TEST(test_arena_alloc_after_reset);
    HU_RUN_TEST(test_tracking_allocator_total);
    HU_RUN_TEST(test_error_string_invalid);
    HU_RUN_TEST(test_str_from_cstr);
    HU_RUN_TEST(test_str_eq_cstr);
    HU_RUN_TEST(test_bytes_from);
    HU_RUN_TEST(test_str_concat_normal);
    HU_RUN_TEST(test_str_join_multiple);
    HU_RUN_TEST(test_arena_large_block);
}
