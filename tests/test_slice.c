#include "human/core/slice.h"
#include "test_framework.h"

static void test_str_lit_macro(void) {
    hu_str_t s = HU_STR_LIT("hello");
    HU_ASSERT_EQ(s.len, 5);
    HU_ASSERT_TRUE(memcmp(s.ptr, "hello", 5) == 0);
}

static void test_str_from_cstr(void) {
    hu_str_t s = hu_str_from_cstr("test");
    HU_ASSERT_EQ(s.len, 4);
    HU_ASSERT_NOT_NULL(s.ptr);
}

static void test_str_null(void) {
    hu_str_t s = HU_STR_NULL;
    HU_ASSERT_TRUE(hu_str_is_empty(s));
    HU_ASSERT_NULL(s.ptr);
}

static void test_str_starts_with(void) {
    HU_ASSERT_TRUE(hu_str_starts_with(HU_STR_LIT("hello world"), HU_STR_LIT("hello")));
    HU_ASSERT_FALSE(hu_str_starts_with(HU_STR_LIT("hello"), HU_STR_LIT("world")));
}

static void test_str_ends_with(void) {
    HU_ASSERT_TRUE(hu_str_ends_with(HU_STR_LIT("hello world"), HU_STR_LIT("world")));
    HU_ASSERT_FALSE(hu_str_ends_with(HU_STR_LIT("hello"), HU_STR_LIT("world")));
}

static void test_bytes_from(void) {
    uint8_t data[] = {0xDE, 0xAD, 0xBE, 0xEF};
    hu_bytes_t b = hu_bytes_from(data, 4);
    HU_ASSERT_EQ(b.len, 4);
    HU_ASSERT_EQ(b.ptr[0], 0xDE);
    HU_ASSERT_EQ(b.ptr[3], 0xEF);
}

static void test_bytes_from_empty(void) {
    uint8_t data[] = {0x00};
    hu_bytes_t b = hu_bytes_from(data, 0);
    HU_ASSERT_EQ(b.len, 0);
}

static void test_str_eq_different_len(void) {
    HU_ASSERT_FALSE(hu_str_eq(HU_STR_LIT("ab"), HU_STR_LIT("abc")));
    HU_ASSERT_FALSE(hu_str_eq(HU_STR_LIT("abc"), HU_STR_LIT("ab")));
}

static void test_str_eq_cstr(void) {
    HU_ASSERT_TRUE(hu_str_eq_cstr(HU_STR_LIT("test"), "test"));
    HU_ASSERT_FALSE(hu_str_eq_cstr(HU_STR_LIT("test"), "other"));
}

static void test_str_from_cstr_null(void) {
    hu_str_t s = hu_str_from_cstr(NULL);
    HU_ASSERT_TRUE(hu_str_is_empty(s));
    HU_ASSERT_EQ(s.len, 0u);
}

static void test_str_starts_with_empty(void) {
    HU_ASSERT_TRUE(hu_str_starts_with(HU_STR_LIT("anything"), HU_STR_LIT("")));
}

static void test_str_ends_with_empty(void) {
    HU_ASSERT_TRUE(hu_str_ends_with(HU_STR_LIT("anything"), HU_STR_LIT("")));
}

static void test_str_trim_no_whitespace(void) {
    hu_str_t s = hu_str_trim(HU_STR_LIT("nospace"));
    HU_ASSERT_TRUE(hu_str_eq(s, HU_STR_LIT("nospace")));
}

static void test_str_lit_empty(void) {
    hu_str_t s = HU_STR_LIT("");
    HU_ASSERT_EQ(s.len, 0u);
    HU_ASSERT_NOT_NULL(s.ptr);
}

static void test_str_is_empty_non_null(void) {
    hu_str_t s = HU_STR_LIT("");
    HU_ASSERT_TRUE(hu_str_is_empty(s));
}

static void test_bytes_from_single(void) {
    uint8_t x = 0x42;
    hu_bytes_t b = hu_bytes_from(&x, 1);
    HU_ASSERT_EQ(b.len, 1);
    HU_ASSERT_EQ(b.ptr[0], 0x42);
}

static void test_str_eq_same_ptr(void) {
    hu_str_t a = HU_STR_LIT("same");
    hu_str_t b = HU_STR_LIT("same");
    HU_ASSERT_TRUE(hu_str_eq(a, b));
}

static void test_str_starts_with_exact_match(void) {
    HU_ASSERT_TRUE(hu_str_starts_with(HU_STR_LIT("exact"), HU_STR_LIT("exact")));
}

static void test_str_ends_with_exact_match(void) {
    HU_ASSERT_TRUE(hu_str_ends_with(HU_STR_LIT("exact"), HU_STR_LIT("exact")));
}

void run_slice_tests(void) {
    HU_TEST_SUITE("slice");
    HU_RUN_TEST(test_str_lit_macro);
    HU_RUN_TEST(test_str_from_cstr);
    HU_RUN_TEST(test_str_null);
    HU_RUN_TEST(test_str_starts_with);
    HU_RUN_TEST(test_str_ends_with);
    HU_RUN_TEST(test_bytes_from);
    HU_RUN_TEST(test_bytes_from_empty);
    HU_RUN_TEST(test_str_eq_different_len);
    HU_RUN_TEST(test_str_eq_cstr);
    HU_RUN_TEST(test_str_from_cstr_null);
    HU_RUN_TEST(test_str_starts_with_empty);
    HU_RUN_TEST(test_str_ends_with_empty);
    HU_RUN_TEST(test_str_trim_no_whitespace);
    HU_RUN_TEST(test_str_lit_empty);
    HU_RUN_TEST(test_str_is_empty_non_null);
    HU_RUN_TEST(test_bytes_from_single);
    HU_RUN_TEST(test_str_eq_same_ptr);
    HU_RUN_TEST(test_str_starts_with_exact_match);
    HU_RUN_TEST(test_str_ends_with_exact_match);
}
