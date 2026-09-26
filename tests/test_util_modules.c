/* Tests for json_util.c, util.c — no network, no file I/O. */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/json_util.h"
#include "human/util.h"
#include "test_framework.h"
#include <string.h>
#include <unistd.h>

/* ─── json_util.c ─────────────────────────────────────────────────────────── */

static void test_json_util_append_string_null_buf_returns_error(void) {
    hu_error_t err = hu_json_util_append_string(NULL, "x");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_json_util_append_string_null_s_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    hu_json_buf_init(&buf, &alloc);
    hu_error_t err = hu_json_util_append_string(&buf, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_json_buf_free(&buf);
}

static void test_json_util_append_string_success(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    HU_ASSERT_EQ(hu_json_buf_init(&buf, &alloc), HU_OK);
    HU_ASSERT_EQ(hu_json_util_append_string(&buf, "hello"), HU_OK);
    HU_ASSERT_TRUE(buf.len >= 5);
    HU_ASSERT_NOT_NULL(strstr(buf.ptr, "hello"));
    hu_json_buf_free(&buf);
}

static void test_json_util_append_key_null_key_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    hu_json_buf_init(&buf, &alloc);
    hu_error_t err = hu_json_util_append_key(&buf, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    hu_json_buf_free(&buf);
}

static void test_json_util_append_key_value_success(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    HU_ASSERT_EQ(hu_json_buf_init(&buf, &alloc), HU_OK);
    HU_ASSERT_EQ(hu_json_util_append_key_value(&buf, "name", "Alice"), HU_OK);
    HU_ASSERT_TRUE(buf.len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf.ptr, "name"));
    HU_ASSERT_NOT_NULL(strstr(buf.ptr, "Alice"));
    hu_json_buf_free(&buf);
}

static void test_json_util_append_key_int_success(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_json_buf_t buf;
    HU_ASSERT_EQ(hu_json_buf_init(&buf, &alloc), HU_OK);
    HU_ASSERT_EQ(hu_json_util_append_key_int(&buf, "count", 42), HU_OK);
    HU_ASSERT_TRUE(buf.len > 0);
    HU_ASSERT_NOT_NULL(strstr(buf.ptr, "count"));
    HU_ASSERT_NOT_NULL(strstr(buf.ptr, "42"));
    hu_json_buf_free(&buf);
}

/* ─── util.c ──────────────────────────────────────────────────────────────── */

static void test_util_trim_empty_string(void) {
    char s[] = "   ";
    size_t n = hu_util_trim(s, 3);
    HU_ASSERT_EQ(n, 0);
    HU_ASSERT_EQ(s[0], '\0');
}

static void test_util_trim_leading_trailing(void) {
    char s[] = "  hello  ";
    size_t n = hu_util_trim(s, 9);
    HU_ASSERT_EQ(n, 5);
    HU_ASSERT_STR_EQ(s, "hello");
}

static void test_util_trim_no_whitespace(void) {
    char s[] = "hello";
    size_t n = hu_util_trim(s, 5);
    HU_ASSERT_EQ(n, 5);
    HU_ASSERT_STR_EQ(s, "hello");
}

static void test_util_trim_null_returns_zero(void) {
    HU_ASSERT_EQ(hu_util_trim(NULL, 10), 0);
}

static void test_util_trim_zero_len_returns_zero(void) {
    char s[] = "  x  ";
    HU_ASSERT_EQ(hu_util_trim(s, 0), 0);
}

static void test_util_strdup_success(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* alloc.ctx may be NULL for system allocator; use alloc ptr as non-null ctx */
    void *ctx = alloc.ctx ? alloc.ctx : (void *)&alloc;
    char *dup = hu_util_strdup(ctx, alloc.alloc, "test_string");
    HU_ASSERT_NOT_NULL(dup);
    HU_ASSERT_STR_EQ(dup, "test_string");
    hu_util_strfree(ctx, alloc.free, dup);
}

static void test_util_strdup_null_s_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_NULL(hu_util_strdup(alloc.ctx, alloc.alloc, NULL));
}

static void test_util_strdup_null_alloc_returns_null(void) {
    HU_ASSERT_NULL(hu_util_strdup(NULL, NULL, "x"));
}

static void test_util_strcasecmp_equal(void) {
    HU_ASSERT_EQ(hu_util_strcasecmp("Hello", "hello"), 0);
    HU_ASSERT_EQ(hu_util_strcasecmp("ABC", "abc"), 0);
}

static void test_util_strcasecmp_less_greater(void) {
    HU_ASSERT_TRUE(hu_util_strcasecmp("a", "b") < 0);
    HU_ASSERT_TRUE(hu_util_strcasecmp("b", "a") > 0);
}

static void test_util_strcasecmp_null_a(void) {
    HU_ASSERT_TRUE(hu_util_strcasecmp(NULL, "x") < 0);
}

static void test_util_strcasecmp_null_b(void) {
    HU_ASSERT_TRUE(hu_util_strcasecmp("x", NULL) > 0);
}

static void test_util_strcasecmp_both_null(void) {
    HU_ASSERT_EQ(hu_util_strcasecmp(NULL, NULL), 0);
}

static void test_util_gen_session_id_returns_non_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *sid = hu_util_gen_session_id(alloc.ctx, alloc.alloc);
    HU_ASSERT_NOT_NULL(sid);
    HU_ASSERT_TRUE(strlen(sid) > 0);
    alloc.free(alloc.ctx, sid, strlen(sid) + 1);
}

static void test_util_gen_session_id_null_alloc_returns_null(void) {
    HU_ASSERT_NULL(hu_util_gen_session_id(NULL, NULL));
}

/* ─── suite ───────────────────────────────────────────────────────────────── */

void run_util_modules_tests(void) {
    HU_TEST_SUITE("util_modules (json_util, util)");

    HU_RUN_TEST(test_json_util_append_string_null_buf_returns_error);
    HU_RUN_TEST(test_json_util_append_string_null_s_returns_error);
    HU_RUN_TEST(test_json_util_append_string_success);
    HU_RUN_TEST(test_json_util_append_key_null_key_returns_error);
    HU_RUN_TEST(test_json_util_append_key_value_success);
    HU_RUN_TEST(test_json_util_append_key_int_success);

    HU_RUN_TEST(test_util_trim_empty_string);
    HU_RUN_TEST(test_util_trim_leading_trailing);
    HU_RUN_TEST(test_util_trim_no_whitespace);
    HU_RUN_TEST(test_util_trim_null_returns_zero);
    HU_RUN_TEST(test_util_trim_zero_len_returns_zero);
    HU_RUN_TEST(test_util_strdup_success);
    HU_RUN_TEST(test_util_strdup_null_s_returns_null);
    HU_RUN_TEST(test_util_strdup_null_alloc_returns_null);
    HU_RUN_TEST(test_util_strcasecmp_equal);
    HU_RUN_TEST(test_util_strcasecmp_less_greater);
    HU_RUN_TEST(test_util_strcasecmp_null_a);
    HU_RUN_TEST(test_util_strcasecmp_null_b);
    HU_RUN_TEST(test_util_strcasecmp_both_null);
    HU_RUN_TEST(test_util_gen_session_id_returns_non_null);
    HU_RUN_TEST(test_util_gen_session_id_null_alloc_returns_null);
}
