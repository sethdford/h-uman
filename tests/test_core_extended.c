/* Core utility edge cases (~30 tests). */
#include "test_framework.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/arena.h"
#include "seaclaw/core/string.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/slice.h"
#include <string.h>
#include <stdio.h>

static void test_arena_multiple_allocs(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_arena_t *arena = sc_arena_create(sys);
    sc_allocator_t alloc = sc_arena_allocator(arena);
    void *p1 = alloc.alloc(alloc.ctx, 16);
    void *p2 = alloc.alloc(alloc.ctx, 32);
    void *p3 = alloc.alloc(alloc.ctx, 64);
    SC_ASSERT_NOT_NULL(p1);
    SC_ASSERT_NOT_NULL(p2);
    SC_ASSERT_NOT_NULL(p3);
    SC_ASSERT(p1 != p2);
    SC_ASSERT(p2 != p3);
    SC_ASSERT_EQ(sc_arena_bytes_used(arena), 112u);
    sc_arena_destroy(arena);
}

static void test_arena_reset(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_arena_t *arena = sc_arena_create(sys);
    sc_allocator_t alloc = sc_arena_allocator(arena);
    alloc.alloc(alloc.ctx, 100);
    alloc.alloc(alloc.ctx, 200);
    SC_ASSERT_EQ(sc_arena_bytes_used(arena), 300u);
    sc_arena_reset(arena);
    SC_ASSERT_EQ(sc_arena_bytes_used(arena), 0u);
    sc_arena_destroy(arena);
}

static void test_string_empty_concat(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_str_concat(&alloc, SC_STR_LIT(""), SC_STR_LIT(""));
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_EQ(strlen(s), 0u);
    sc_str_free(&alloc, s);
}

static void test_string_null_inputs(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_strdup(&alloc, NULL);
    SC_ASSERT_NULL(s);
}

static void test_string_very_long(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[10000];
    for (size_t i = 0; i < sizeof(buf) - 1; i++) buf[i] = 'x';
    buf[sizeof(buf) - 1] = '\0';
    char *s = sc_strdup(&alloc, buf);
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_EQ(strlen(s), 9999u);
    sc_str_free(&alloc, s);
}

static void test_allocator_zero_size(void) {
    sc_allocator_t alloc = sc_system_allocator();
    void *p = alloc.alloc(alloc.ctx, 0);
    if (p) alloc.free(alloc.ctx, p, 0);
}

static void test_allocator_realloc_grow_shrink(void) {
    sc_allocator_t alloc = sc_system_allocator();
    void *p = alloc.alloc(alloc.ctx, 64);
    SC_ASSERT_NOT_NULL(p);
    memset(p, 0x42, 64);
    p = alloc.realloc(alloc.ctx, p, 64, 256);
    SC_ASSERT_NOT_NULL(p);
    unsigned char *b = (unsigned char *)p;
    for (int i = 0; i < 64; i++) SC_ASSERT_EQ(b[i], 0x42);
    p = alloc.realloc(alloc.ctx, p, 256, 32);
    SC_ASSERT_NOT_NULL(p);
    alloc.free(alloc.ctx, p, 32);
}

static void test_tracking_allocator_reports_leak_count(void) {
    sc_tracking_allocator_t *ta = sc_tracking_allocator_create();
    sc_allocator_t alloc = sc_tracking_allocator_allocator(ta);
    void *p = alloc.alloc(alloc.ctx, 100);
    SC_ASSERT_EQ(sc_tracking_allocator_leaks(ta), 1u);
    alloc.free(alloc.ctx, p, 100);
    SC_ASSERT_EQ(sc_tracking_allocator_leaks(ta), 0u);
    sc_tracking_allocator_destroy(ta);
}

static void test_error_string_all_codes(void) {
    const char *s0 = sc_error_string(SC_OK);
    const char *s1 = sc_error_string(SC_ERR_OUT_OF_MEMORY);
    const char *s2 = sc_error_string(SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_NOT_NULL(s0);
    SC_ASSERT_NOT_NULL(s1);
    SC_ASSERT_NOT_NULL(s2);
    SC_ASSERT(strlen(s0) > 0);
}

static void test_strndup_zero_len(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_strndup(&alloc, "hello", 0);
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_EQ(strlen(s), 0u);
    sc_str_free(&alloc, s);
}

static void test_str_join_empty_parts(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_str_t parts[] = { SC_STR_LIT("") };
    char *s = sc_str_join(&alloc, parts, 1, SC_STR_LIT(", "));
    SC_ASSERT_NOT_NULL(s);
    sc_str_free(&alloc, s);
}

static void test_str_join_single(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_str_t parts[] = { SC_STR_LIT("a") };
    char *s = sc_str_join(&alloc, parts, 1, SC_STR_LIT("-"));
    SC_ASSERT_STR_EQ(s, "a");
    sc_str_free(&alloc, s);
}

static void test_str_contains_empty_needle(void) {
    SC_ASSERT_TRUE(sc_str_contains(SC_STR_LIT("hello"), SC_STR_LIT("")));
}

static void test_str_index_of(void) {
    int idx = sc_str_index_of(SC_STR_LIT("hello world"), SC_STR_LIT("world"));
    SC_ASSERT_EQ(idx, 6);
    idx = sc_str_index_of(SC_STR_LIT("hello"), SC_STR_LIT("xyz"));
    SC_ASSERT_EQ(idx, -1);
}

static void test_str_starts_with(void) {
    SC_ASSERT_TRUE(sc_str_starts_with(SC_STR_LIT("hello"), SC_STR_LIT("hell")));
    SC_ASSERT_FALSE(sc_str_starts_with(SC_STR_LIT("hello"), SC_STR_LIT("ell")));
}

static void test_str_ends_with(void) {
    SC_ASSERT_TRUE(sc_str_ends_with(SC_STR_LIT("hello"), SC_STR_LIT("llo")));
    SC_ASSERT_FALSE(sc_str_ends_with(SC_STR_LIT("hello"), SC_STR_LIT("ell")));
}

static void test_str_trim_all_space(void) {
    sc_str_t t = sc_str_trim(SC_STR_LIT("   "));
    SC_ASSERT_EQ(t.len, 0u);
}

static void test_str_is_empty(void) {
    SC_ASSERT_TRUE(sc_str_is_empty(SC_STR_NULL));
    SC_ASSERT_TRUE(sc_str_is_empty(SC_STR_LIT("")));
    SC_ASSERT_FALSE(sc_str_is_empty(SC_STR_LIT("x")));
}

static void test_sprintf_format(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_sprintf(&alloc, "%d %s", 42, "test");
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_TRUE(strstr(s, "42") != NULL);
    SC_ASSERT_TRUE(strstr(s, "test") != NULL);
    sc_str_free(&alloc, s);
}

static void test_arena_alloc_after_reset(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_arena_t *arena = sc_arena_create(sys);
    sc_allocator_t alloc = sc_arena_allocator(arena);
    alloc.alloc(alloc.ctx, 50);
    sc_arena_reset(arena);
    void *p = alloc.alloc(alloc.ctx, 25);
    SC_ASSERT_NOT_NULL(p);
    SC_ASSERT_EQ(sc_arena_bytes_used(arena), 25u);
    sc_arena_destroy(arena);
}

static void test_tracking_allocator_total(void) {
    sc_tracking_allocator_t *ta = sc_tracking_allocator_create();
    sc_allocator_t alloc = sc_tracking_allocator_allocator(ta);
    alloc.alloc(alloc.ctx, 10);
    alloc.alloc(alloc.ctx, 20);
    SC_ASSERT_EQ(sc_tracking_allocator_total_allocated(ta), 30u);
    sc_tracking_allocator_destroy(ta);
}

static void test_error_string_invalid(void) {
    const char *s = sc_error_string((sc_error_t)999);
    SC_ASSERT_NOT_NULL(s);
}

static void test_str_from_cstr(void) {
    sc_str_t s = sc_str_from_cstr("hello");
    SC_ASSERT_EQ(s.len, 5u);
    SC_ASSERT_STR_EQ(s.ptr, "hello");
}

static void test_str_eq_cstr(void) {
    SC_ASSERT_TRUE(sc_str_eq_cstr(SC_STR_LIT("abc"), "abc"));
    SC_ASSERT_FALSE(sc_str_eq_cstr(SC_STR_LIT("abc"), "abd"));
}

static void test_bytes_from(void) {
    uint8_t data[] = {1, 2, 3};
    sc_bytes_t b = sc_bytes_from(data, 3);
    SC_ASSERT_EQ(b.len, 3u);
    SC_ASSERT_EQ(b.ptr[0], 1);
}

static void test_str_concat_normal(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_str_concat(&alloc, SC_STR_LIT("foo"), SC_STR_LIT("bar"));
    SC_ASSERT_STR_EQ(s, "foobar");
    sc_str_free(&alloc, s);
}

static void test_str_join_multiple(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_str_t parts[] = { SC_STR_LIT("a"), SC_STR_LIT("b"), SC_STR_LIT("c") };
    char *s = sc_str_join(&alloc, parts, 3, SC_STR_LIT("|"));
    SC_ASSERT_STR_EQ(s, "a|b|c");
    sc_str_free(&alloc, s);
}

static void test_arena_large_block(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_arena_t *arena = sc_arena_create(sys);
    sc_allocator_t alloc = sc_arena_allocator(arena);
    void *p = alloc.alloc(alloc.ctx, 65536);
    SC_ASSERT_NOT_NULL(p);
    sc_arena_destroy(arena);
}

void run_core_extended_tests(void) {
    SC_TEST_SUITE("Core Extended");
    SC_RUN_TEST(test_arena_multiple_allocs);
    SC_RUN_TEST(test_arena_reset);
    SC_RUN_TEST(test_string_empty_concat);
    SC_RUN_TEST(test_string_null_inputs);
    SC_RUN_TEST(test_string_very_long);
    SC_RUN_TEST(test_allocator_zero_size);
    SC_RUN_TEST(test_allocator_realloc_grow_shrink);
    SC_RUN_TEST(test_tracking_allocator_reports_leak_count);
    SC_RUN_TEST(test_error_string_all_codes);
    SC_RUN_TEST(test_strndup_zero_len);
    SC_RUN_TEST(test_str_join_empty_parts);
    SC_RUN_TEST(test_str_join_single);
    SC_RUN_TEST(test_str_contains_empty_needle);
    SC_RUN_TEST(test_str_index_of);
    SC_RUN_TEST(test_str_starts_with);
    SC_RUN_TEST(test_str_ends_with);
    SC_RUN_TEST(test_str_trim_all_space);
    SC_RUN_TEST(test_str_is_empty);
    SC_RUN_TEST(test_sprintf_format);
    SC_RUN_TEST(test_arena_alloc_after_reset);
    SC_RUN_TEST(test_tracking_allocator_total);
    SC_RUN_TEST(test_error_string_invalid);
    SC_RUN_TEST(test_str_from_cstr);
    SC_RUN_TEST(test_str_eq_cstr);
    SC_RUN_TEST(test_bytes_from);
    SC_RUN_TEST(test_str_concat_normal);
    SC_RUN_TEST(test_str_join_multiple);
    SC_RUN_TEST(test_arena_large_block);
}
