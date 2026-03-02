#include "test_framework.h"
#include "seaclaw/core/slice.h"

static void test_str_lit_macro(void) {
    sc_str_t s = SC_STR_LIT("hello");
    SC_ASSERT_EQ(s.len, 5);
    SC_ASSERT_TRUE(memcmp(s.ptr, "hello", 5) == 0);
}

static void test_str_from_cstr(void) {
    sc_str_t s = sc_str_from_cstr("test");
    SC_ASSERT_EQ(s.len, 4);
    SC_ASSERT_NOT_NULL(s.ptr);
}

static void test_str_null(void) {
    sc_str_t s = SC_STR_NULL;
    SC_ASSERT_TRUE(sc_str_is_empty(s));
    SC_ASSERT_NULL(s.ptr);
}

static void test_str_starts_with(void) {
    SC_ASSERT_TRUE(sc_str_starts_with(SC_STR_LIT("hello world"), SC_STR_LIT("hello")));
    SC_ASSERT_FALSE(sc_str_starts_with(SC_STR_LIT("hello"), SC_STR_LIT("world")));
}

static void test_str_ends_with(void) {
    SC_ASSERT_TRUE(sc_str_ends_with(SC_STR_LIT("hello world"), SC_STR_LIT("world")));
    SC_ASSERT_FALSE(sc_str_ends_with(SC_STR_LIT("hello"), SC_STR_LIT("world")));
}

static void test_bytes_from(void) {
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    sc_bytes_t b = sc_bytes_from(data, 4);
    SC_ASSERT_EQ(b.len, 4);
    SC_ASSERT_EQ(b.ptr[0], 0xDE);
    SC_ASSERT_EQ(b.ptr[3], 0xEF);
}

static void test_bytes_from_empty(void) {
    uint8_t data[] = {0x00};
    sc_bytes_t b = sc_bytes_from(data, 0);
    SC_ASSERT_EQ(b.len, 0);
}

static void test_str_eq_different_len(void) {
    SC_ASSERT_FALSE(sc_str_eq(SC_STR_LIT("ab"), SC_STR_LIT("abc")));
    SC_ASSERT_FALSE(sc_str_eq(SC_STR_LIT("abc"), SC_STR_LIT("ab")));
}

static void test_str_eq_cstr(void) {
    SC_ASSERT_TRUE(sc_str_eq_cstr(SC_STR_LIT("test"), "test"));
    SC_ASSERT_FALSE(sc_str_eq_cstr(SC_STR_LIT("test"), "other"));
}

static void test_str_from_cstr_null(void) {
    sc_str_t s = sc_str_from_cstr(NULL);
    SC_ASSERT_TRUE(sc_str_is_empty(s));
    SC_ASSERT_EQ(s.len, 0u);
}

static void test_str_starts_with_empty(void) {
    SC_ASSERT_TRUE(sc_str_starts_with(SC_STR_LIT("anything"), SC_STR_LIT("")));
}

static void test_str_ends_with_empty(void) {
    SC_ASSERT_TRUE(sc_str_ends_with(SC_STR_LIT("anything"), SC_STR_LIT("")));
}

static void test_str_trim_no_whitespace(void) {
    sc_str_t s = sc_str_trim(SC_STR_LIT("nospace"));
    SC_ASSERT_TRUE(sc_str_eq(s, SC_STR_LIT("nospace")));
}

static void test_str_lit_empty(void) {
    sc_str_t s = SC_STR_LIT("");
    SC_ASSERT_EQ(s.len, 0u);
    SC_ASSERT_NOT_NULL(s.ptr);
}

static void test_str_is_empty_non_null(void) {
    sc_str_t s = SC_STR_LIT("");
    SC_ASSERT_TRUE(sc_str_is_empty(s));
}

static void test_bytes_from_single(void) {
    uint8_t x = 0x42;
    sc_bytes_t b = sc_bytes_from(&x, 1);
    SC_ASSERT_EQ(b.len, 1);
    SC_ASSERT_EQ(b.ptr[0], 0x42);
}

static void test_str_eq_same_ptr(void) {
    sc_str_t a = SC_STR_LIT("same");
    sc_str_t b = SC_STR_LIT("same");
    SC_ASSERT_TRUE(sc_str_eq(a, b));
}

static void test_str_starts_with_exact_match(void) {
    SC_ASSERT_TRUE(sc_str_starts_with(SC_STR_LIT("exact"), SC_STR_LIT("exact")));
}

static void test_str_ends_with_exact_match(void) {
    SC_ASSERT_TRUE(sc_str_ends_with(SC_STR_LIT("exact"), SC_STR_LIT("exact")));
}

void run_slice_tests(void) {
    SC_TEST_SUITE("slice");
    SC_RUN_TEST(test_str_lit_macro);
    SC_RUN_TEST(test_str_from_cstr);
    SC_RUN_TEST(test_str_null);
    SC_RUN_TEST(test_str_starts_with);
    SC_RUN_TEST(test_str_ends_with);
    SC_RUN_TEST(test_bytes_from);
    SC_RUN_TEST(test_bytes_from_empty);
    SC_RUN_TEST(test_str_eq_different_len);
    SC_RUN_TEST(test_str_eq_cstr);
    SC_RUN_TEST(test_str_from_cstr_null);
    SC_RUN_TEST(test_str_starts_with_empty);
    SC_RUN_TEST(test_str_ends_with_empty);
    SC_RUN_TEST(test_str_trim_no_whitespace);
    SC_RUN_TEST(test_str_lit_empty);
    SC_RUN_TEST(test_str_is_empty_non_null);
    SC_RUN_TEST(test_bytes_from_single);
    SC_RUN_TEST(test_str_eq_same_ptr);
    SC_RUN_TEST(test_str_starts_with_exact_match);
    SC_RUN_TEST(test_str_ends_with_exact_match);
}
