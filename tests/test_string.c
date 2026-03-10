#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"

static void test_strdup(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_strdup(&alloc, "hello");
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, "hello");
    hu_str_free(&alloc, s);
}

static void test_strndup(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_strndup(&alloc, "hello world", 5);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, "hello");
    hu_str_free(&alloc, s);
}

static void test_str_concat(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_str_concat(&alloc, HU_STR_LIT("hello "), HU_STR_LIT("world"));
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, "hello world");
    hu_str_free(&alloc, s);
}

static void test_str_join(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_str_t parts[] = {HU_STR_LIT("a"), HU_STR_LIT("b"), HU_STR_LIT("c")};
    char *s = hu_str_join(&alloc, parts, 3, HU_STR_LIT(", "));
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, "a, b, c");
    hu_str_free(&alloc, s);
}

static void test_sprintf(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_sprintf(&alloc, "%s v%d.%d", "human", 0, 1);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, "human v0.1");
    hu_str_free(&alloc, s);
}

static void test_str_contains(void) {
    HU_ASSERT_TRUE(hu_str_contains(HU_STR_LIT("hello world"), HU_STR_LIT("world")));
    HU_ASSERT_FALSE(hu_str_contains(HU_STR_LIT("hello world"), HU_STR_LIT("xyz")));
    HU_ASSERT_TRUE(hu_str_contains(HU_STR_LIT("hello"), HU_STR_LIT("")));
}

static void test_str_eq(void) {
    HU_ASSERT_TRUE(hu_str_eq(HU_STR_LIT("abc"), HU_STR_LIT("abc")));
    HU_ASSERT_FALSE(hu_str_eq(HU_STR_LIT("abc"), HU_STR_LIT("def")));
    HU_ASSERT_FALSE(hu_str_eq(HU_STR_LIT("abc"), HU_STR_LIT("ab")));
}

static void test_str_trim(void) {
    hu_str_t trimmed = hu_str_trim(HU_STR_LIT("  hello  "));
    HU_ASSERT_TRUE(hu_str_eq(trimmed, HU_STR_LIT("hello")));
}

static void test_strdup_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_strdup(&alloc, NULL);
    HU_ASSERT_NULL(s);
}

static void test_strdup_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_strdup(&alloc, "");
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, "");
    hu_str_free(&alloc, s);
}

static void test_strndup_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_strndup(&alloc, "", 0);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, "");
    hu_str_free(&alloc, s);
}

static void test_strndup_n_zero(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_strndup(&alloc, "hello", 0);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, "");
    hu_str_free(&alloc, s);
}

static void test_strndup_n_exceeds_len(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_strndup(&alloc, "hi", 10);
    HU_ASSERT_STR_EQ(s, "hi");
    hu_str_free(&alloc, s);
}

static void test_sprintf_int(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_sprintf(&alloc, "%d", -42);
    HU_ASSERT_STR_EQ(s, "-42");
    hu_str_free(&alloc, s);
}

static void test_sprintf_float(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_sprintf(&alloc, "%.2f", 3.14);
    HU_ASSERT_TRUE(strstr(s, "3.14") != NULL);
    hu_str_free(&alloc, s);
}

static void test_sprintf_multiple(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_sprintf(&alloc, "%s=%d", "x", 99);
    HU_ASSERT_STR_EQ(s, "x=99");
    hu_str_free(&alloc, s);
}

static void test_str_trim_leading_tabs(void) {
    hu_str_t trimmed = hu_str_trim(HU_STR_LIT("\t\tfoo"));
    HU_ASSERT_TRUE(hu_str_eq(trimmed, HU_STR_LIT("foo")));
}

static void test_str_trim_trailing_newlines(void) {
    hu_str_t trimmed = hu_str_trim(HU_STR_LIT("bar\n\n"));
    HU_ASSERT_TRUE(hu_str_eq(trimmed, HU_STR_LIT("bar")));
}

static void test_str_trim_all_whitespace(void) {
    hu_str_t trimmed = hu_str_trim(HU_STR_LIT("   \t\n\r  "));
    HU_ASSERT_EQ(trimmed.len, 0u);
}

static void test_str_starts_with_prefix(void) {
    HU_ASSERT_TRUE(hu_str_starts_with(HU_STR_LIT("prefix_suffix"), HU_STR_LIT("prefix")));
    HU_ASSERT_FALSE(hu_str_starts_with(HU_STR_LIT("short"), HU_STR_LIT("longer")));
}

static void test_str_ends_with_suffix(void) {
    HU_ASSERT_TRUE(hu_str_ends_with(HU_STR_LIT("file.txt"), HU_STR_LIT(".txt")));
    HU_ASSERT_FALSE(hu_str_ends_with(HU_STR_LIT("x"), HU_STR_LIT(".txt")));
}

static void test_str_index_of(void) {
    HU_ASSERT_EQ(hu_str_index_of(HU_STR_LIT("hello world"), HU_STR_LIT("world")), 6);
    HU_ASSERT_EQ(hu_str_index_of(HU_STR_LIT("hello"), HU_STR_LIT("xyz")), -1);
    HU_ASSERT_EQ(hu_str_index_of(HU_STR_LIT("hello"), HU_STR_LIT("")), 0);
}

static void test_str_dup(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_str_dup(&alloc, HU_STR_LIT("dup me"));
    HU_ASSERT_STR_EQ(s, "dup me");
    hu_str_free(&alloc, s);
}

static void test_str_dup_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_str_dup(&alloc, HU_STR_NULL);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_STR_EQ(s, "");
    hu_str_free(&alloc, s);
}

static void test_str_join_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_str_t parts[] = {HU_STR_LIT("a")};
    char *s = hu_str_join(&alloc, parts, 0, HU_STR_LIT(", "));
    HU_ASSERT_STR_EQ(s, "");
    hu_str_free(&alloc, s);
}

static void test_str_join_single(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_str_t parts[] = {HU_STR_LIT("only")};
    char *s = hu_str_join(&alloc, parts, 1, HU_STR_LIT("|"));
    HU_ASSERT_STR_EQ(s, "only");
    hu_str_free(&alloc, s);
}

static void test_str_concat_empty_first(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_str_concat(&alloc, HU_STR_NULL, HU_STR_LIT("world"));
    HU_ASSERT_STR_EQ(s, "world");
    hu_str_free(&alloc, s);
}

static void test_str_concat_empty_second(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *s = hu_str_concat(&alloc, HU_STR_LIT("hello"), HU_STR_NULL);
    HU_ASSERT_STR_EQ(s, "hello");
    hu_str_free(&alloc, s);
}

static void test_str_contains_empty_needle(void) {
    HU_ASSERT_TRUE(hu_str_contains(HU_STR_LIT("any"), HU_STR_LIT("")));
}

static void test_str_contains_self(void) {
    HU_ASSERT_TRUE(hu_str_contains(HU_STR_LIT("hello"), HU_STR_LIT("hello")));
}

void run_string_tests(void) {
    HU_TEST_SUITE("string");
    HU_RUN_TEST(test_strdup);
    HU_RUN_TEST(test_strndup);
    HU_RUN_TEST(test_str_concat);
    HU_RUN_TEST(test_str_join);
    HU_RUN_TEST(test_sprintf);
    HU_RUN_TEST(test_str_contains);
    HU_RUN_TEST(test_str_eq);
    HU_RUN_TEST(test_str_trim);
    HU_RUN_TEST(test_strdup_null);
    HU_RUN_TEST(test_strdup_empty);
    HU_RUN_TEST(test_strndup_empty);
    HU_RUN_TEST(test_strndup_n_zero);
    HU_RUN_TEST(test_strndup_n_exceeds_len);
    HU_RUN_TEST(test_sprintf_int);
    HU_RUN_TEST(test_sprintf_float);
    HU_RUN_TEST(test_sprintf_multiple);
    HU_RUN_TEST(test_str_trim_leading_tabs);
    HU_RUN_TEST(test_str_trim_trailing_newlines);
    HU_RUN_TEST(test_str_trim_all_whitespace);
    HU_RUN_TEST(test_str_starts_with_prefix);
    HU_RUN_TEST(test_str_ends_with_suffix);
    HU_RUN_TEST(test_str_index_of);
    HU_RUN_TEST(test_str_dup);
    HU_RUN_TEST(test_str_dup_empty);
    HU_RUN_TEST(test_str_join_empty);
    HU_RUN_TEST(test_str_join_single);
    HU_RUN_TEST(test_str_concat_empty_first);
    HU_RUN_TEST(test_str_concat_empty_second);
    HU_RUN_TEST(test_str_contains_empty_needle);
    HU_RUN_TEST(test_str_contains_self);
}
