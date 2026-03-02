#include "test_framework.h"
#include "seaclaw/core/string.h"
#include "seaclaw/core/allocator.h"

static void test_strdup(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_strdup(&alloc, "hello");
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_STR_EQ(s, "hello");
    sc_str_free(&alloc, s);
}

static void test_strndup(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_strndup(&alloc, "hello world", 5);
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_STR_EQ(s, "hello");
    sc_str_free(&alloc, s);
}

static void test_str_concat(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_str_concat(&alloc, SC_STR_LIT("hello "), SC_STR_LIT("world"));
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_STR_EQ(s, "hello world");
    sc_str_free(&alloc, s);
}

static void test_str_join(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_str_t parts[] = { SC_STR_LIT("a"), SC_STR_LIT("b"), SC_STR_LIT("c") };
    char *s = sc_str_join(&alloc, parts, 3, SC_STR_LIT(", "));
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_STR_EQ(s, "a, b, c");
    sc_str_free(&alloc, s);
}

static void test_sprintf(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_sprintf(&alloc, "%s v%d.%d", "seaclaw", 0, 1);
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_STR_EQ(s, "seaclaw v0.1");
    sc_str_free(&alloc, s);
}

static void test_str_contains(void) {
    SC_ASSERT_TRUE(sc_str_contains(SC_STR_LIT("hello world"), SC_STR_LIT("world")));
    SC_ASSERT_FALSE(sc_str_contains(SC_STR_LIT("hello world"), SC_STR_LIT("xyz")));
    SC_ASSERT_TRUE(sc_str_contains(SC_STR_LIT("hello"), SC_STR_LIT("")));
}

static void test_str_eq(void) {
    SC_ASSERT_TRUE(sc_str_eq(SC_STR_LIT("abc"), SC_STR_LIT("abc")));
    SC_ASSERT_FALSE(sc_str_eq(SC_STR_LIT("abc"), SC_STR_LIT("def")));
    SC_ASSERT_FALSE(sc_str_eq(SC_STR_LIT("abc"), SC_STR_LIT("ab")));
}

static void test_str_trim(void) {
    sc_str_t trimmed = sc_str_trim(SC_STR_LIT("  hello  "));
    SC_ASSERT_TRUE(sc_str_eq(trimmed, SC_STR_LIT("hello")));
}

static void test_strdup_null(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_strdup(&alloc, NULL);
    SC_ASSERT_NULL(s);
}

static void test_strdup_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_strdup(&alloc, "");
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_STR_EQ(s, "");
    sc_str_free(&alloc, s);
}

static void test_strndup_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_strndup(&alloc, "", 0);
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_STR_EQ(s, "");
    sc_str_free(&alloc, s);
}

static void test_strndup_n_zero(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_strndup(&alloc, "hello", 0);
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_STR_EQ(s, "");
    sc_str_free(&alloc, s);
}

static void test_strndup_n_exceeds_len(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_strndup(&alloc, "hi", 10);
    SC_ASSERT_STR_EQ(s, "hi");
    sc_str_free(&alloc, s);
}

static void test_sprintf_int(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_sprintf(&alloc, "%d", -42);
    SC_ASSERT_STR_EQ(s, "-42");
    sc_str_free(&alloc, s);
}

static void test_sprintf_float(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_sprintf(&alloc, "%.2f", 3.14);
    SC_ASSERT_TRUE(strstr(s, "3.14") != NULL);
    sc_str_free(&alloc, s);
}

static void test_sprintf_multiple(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_sprintf(&alloc, "%s=%d", "x", 99);
    SC_ASSERT_STR_EQ(s, "x=99");
    sc_str_free(&alloc, s);
}

static void test_str_trim_leading_tabs(void) {
    sc_str_t trimmed = sc_str_trim(SC_STR_LIT("\t\tfoo"));
    SC_ASSERT_TRUE(sc_str_eq(trimmed, SC_STR_LIT("foo")));
}

static void test_str_trim_trailing_newlines(void) {
    sc_str_t trimmed = sc_str_trim(SC_STR_LIT("bar\n\n"));
    SC_ASSERT_TRUE(sc_str_eq(trimmed, SC_STR_LIT("bar")));
}

static void test_str_trim_all_whitespace(void) {
    sc_str_t trimmed = sc_str_trim(SC_STR_LIT("   \t\n\r  "));
    SC_ASSERT_EQ(trimmed.len, 0u);
}

static void test_str_starts_with_prefix(void) {
    SC_ASSERT_TRUE(sc_str_starts_with(SC_STR_LIT("prefix_suffix"), SC_STR_LIT("prefix")));
    SC_ASSERT_FALSE(sc_str_starts_with(SC_STR_LIT("short"), SC_STR_LIT("longer")));
}

static void test_str_ends_with_suffix(void) {
    SC_ASSERT_TRUE(sc_str_ends_with(SC_STR_LIT("file.txt"), SC_STR_LIT(".txt")));
    SC_ASSERT_FALSE(sc_str_ends_with(SC_STR_LIT("x"), SC_STR_LIT(".txt")));
}

static void test_str_index_of(void) {
    SC_ASSERT_EQ(sc_str_index_of(SC_STR_LIT("hello world"), SC_STR_LIT("world")), 6);
    SC_ASSERT_EQ(sc_str_index_of(SC_STR_LIT("hello"), SC_STR_LIT("xyz")), -1);
    SC_ASSERT_EQ(sc_str_index_of(SC_STR_LIT("hello"), SC_STR_LIT("")), 0);
}

static void test_str_dup(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_str_dup(&alloc, SC_STR_LIT("dup me"));
    SC_ASSERT_STR_EQ(s, "dup me");
    sc_str_free(&alloc, s);
}

static void test_str_dup_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_str_dup(&alloc, SC_STR_NULL);
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_STR_EQ(s, "");
    sc_str_free(&alloc, s);
}

static void test_str_join_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_str_t parts[] = { SC_STR_LIT("a") };
    char *s = sc_str_join(&alloc, parts, 0, SC_STR_LIT(", "));
    SC_ASSERT_STR_EQ(s, "");
    sc_str_free(&alloc, s);
}

static void test_str_join_single(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_str_t parts[] = { SC_STR_LIT("only") };
    char *s = sc_str_join(&alloc, parts, 1, SC_STR_LIT("|"));
    SC_ASSERT_STR_EQ(s, "only");
    sc_str_free(&alloc, s);
}

static void test_str_concat_empty_first(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_str_concat(&alloc, SC_STR_NULL, SC_STR_LIT("world"));
    SC_ASSERT_STR_EQ(s, "world");
    sc_str_free(&alloc, s);
}

static void test_str_concat_empty_second(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *s = sc_str_concat(&alloc, SC_STR_LIT("hello"), SC_STR_NULL);
    SC_ASSERT_STR_EQ(s, "hello");
    sc_str_free(&alloc, s);
}

static void test_str_contains_empty_needle(void) {
    SC_ASSERT_TRUE(sc_str_contains(SC_STR_LIT("any"), SC_STR_LIT("")));
}

static void test_str_contains_self(void) {
    SC_ASSERT_TRUE(sc_str_contains(SC_STR_LIT("hello"), SC_STR_LIT("hello")));
}

void run_string_tests(void) {
    SC_TEST_SUITE("string");
    SC_RUN_TEST(test_strdup);
    SC_RUN_TEST(test_strndup);
    SC_RUN_TEST(test_str_concat);
    SC_RUN_TEST(test_str_join);
    SC_RUN_TEST(test_sprintf);
    SC_RUN_TEST(test_str_contains);
    SC_RUN_TEST(test_str_eq);
    SC_RUN_TEST(test_str_trim);
    SC_RUN_TEST(test_strdup_null);
    SC_RUN_TEST(test_strdup_empty);
    SC_RUN_TEST(test_strndup_empty);
    SC_RUN_TEST(test_strndup_n_zero);
    SC_RUN_TEST(test_strndup_n_exceeds_len);
    SC_RUN_TEST(test_sprintf_int);
    SC_RUN_TEST(test_sprintf_float);
    SC_RUN_TEST(test_sprintf_multiple);
    SC_RUN_TEST(test_str_trim_leading_tabs);
    SC_RUN_TEST(test_str_trim_trailing_newlines);
    SC_RUN_TEST(test_str_trim_all_whitespace);
    SC_RUN_TEST(test_str_starts_with_prefix);
    SC_RUN_TEST(test_str_ends_with_suffix);
    SC_RUN_TEST(test_str_index_of);
    SC_RUN_TEST(test_str_dup);
    SC_RUN_TEST(test_str_dup_empty);
    SC_RUN_TEST(test_str_join_empty);
    SC_RUN_TEST(test_str_join_single);
    SC_RUN_TEST(test_str_concat_empty_first);
    SC_RUN_TEST(test_str_concat_empty_second);
    SC_RUN_TEST(test_str_contains_empty_needle);
    SC_RUN_TEST(test_str_contains_self);
}
