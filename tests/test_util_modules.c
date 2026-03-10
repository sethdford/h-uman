/* Tests for status.c, state.c, http_util.c, json_util.c, util.c — no network, minimal file I/O
 * (/tmp only). */
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/http_util.h"
#include "human/json_util.h"
#include "human/state.h"
#include "human/status.h"
#include "human/util.h"
#include "test_framework.h"
#include <string.h>
#include <unistd.h>

/* ─── status.c ─────────────────────────────────────────────────────────────── */

static void test_status_null_buf_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_status_run(&alloc, NULL, 256);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_status_small_buf_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[64];
    hu_error_t err = hu_status_run(&alloc, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_status_valid_buf_writes_output(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[512] = {0};
    hu_error_t err = hu_status_run(&alloc, buf, sizeof(buf));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(strstr(buf, "Human"));
    HU_ASSERT_NOT_NULL(strstr(buf, "Version"));
}

static void test_status_output_contains_version_or_no_config(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[512] = {0};
    hu_status_run(&alloc, buf, sizeof(buf));
    int has_version = (strstr(buf, "Version") != NULL);
    int has_no_config = (strstr(buf, "no config") != NULL);
    HU_ASSERT_TRUE(has_version || has_no_config);
}

/* ─── state.c ─────────────────────────────────────────────────────────────── */

static void test_state_manager_init_deinit_lifecycle(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_state_manager_t mgr;
    HU_ASSERT_EQ(hu_state_manager_init(&mgr, &alloc, NULL), HU_OK);
    HU_ASSERT_EQ(mgr.process_state, HU_PROCESS_STATE_STARTING);
    HU_ASSERT_NULL(mgr.state_path);
    hu_state_manager_deinit(&mgr);
}

static void test_state_manager_init_null_mgr_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_state_manager_init(NULL, &alloc, "/tmp/state.json");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_state_manager_init_null_alloc_returns_error(void) {
    hu_state_manager_t mgr;
    hu_error_t err = hu_state_manager_init(&mgr, NULL, "/tmp/state.json");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_state_set_get_process(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_state_manager_t mgr;
    hu_state_manager_init(&mgr, &alloc, NULL);

    hu_state_set_process(&mgr, HU_PROCESS_STATE_RUNNING);
    HU_ASSERT_EQ(hu_state_get_process(&mgr), HU_PROCESS_STATE_RUNNING);

    hu_state_set_process(&mgr, HU_PROCESS_STATE_STOPPING);
    HU_ASSERT_EQ(hu_state_get_process(&mgr), HU_PROCESS_STATE_STOPPING);

    hu_state_manager_deinit(&mgr);
}

static void test_state_get_process_null_returns_stopped(void) {
    HU_ASSERT_EQ(hu_state_get_process(NULL), HU_PROCESS_STATE_STOPPED);
}

static void test_state_set_get_last_channel(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_state_manager_t mgr;
    hu_state_manager_init(&mgr, &alloc, NULL);

    hu_state_set_last_channel(&mgr, "cli", "chat_abc");
    char ch[HU_STATE_CHANNEL_LEN], cid[HU_STATE_CHAT_ID_LEN];
    hu_state_get_last_channel(&mgr, ch, sizeof(ch), cid, sizeof(cid));
    HU_ASSERT_STR_EQ(ch, "cli");
    HU_ASSERT_STR_EQ(cid, "chat_abc");

    hu_state_manager_deinit(&mgr);
}

static void test_state_default_path_returns_workspace_state_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *p = hu_state_default_path(&alloc, "/tmp/workspace");
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_NOT_NULL(strstr(p, "state.json"));
    HU_ASSERT_NOT_NULL(strstr(p, "/tmp/workspace"));
    alloc.free(alloc.ctx, p, strlen(p) + 1);
}

static void test_state_default_path_null_alloc_returns_null(void) {
    HU_ASSERT_NULL(hu_state_default_path(NULL, "/tmp/ws"));
}

static void test_state_default_path_null_workspace_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_NULL(hu_state_default_path(&alloc, NULL));
}

static void test_state_save_load_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char path_buf[256];
    snprintf(path_buf, sizeof(path_buf), "/tmp/human_state_test_%u.json", (unsigned)getpid());
    hu_state_manager_t mgr;
    hu_state_manager_init(&mgr, &alloc, path_buf);
    hu_state_set_last_channel(&mgr, "test_channel", "test_chat_id");

    HU_ASSERT_EQ(hu_state_save(&mgr), HU_OK);
    hu_state_set_last_channel(&mgr, "", "");
    HU_ASSERT_EQ(hu_state_load(&mgr), HU_OK);

    char ch[HU_STATE_CHANNEL_LEN], cid[HU_STATE_CHAT_ID_LEN];
    hu_state_get_last_channel(&mgr, ch, sizeof(ch), cid, sizeof(cid));
    HU_ASSERT_STR_EQ(ch, "test_channel");
    HU_ASSERT_STR_EQ(cid, "test_chat_id");

    hu_state_manager_deinit(&mgr);
    remove(path_buf);
}

/* ─── http_util.c ───────────────────────────────────────────────────────────
 * No network — test only argument validation. */

static void test_http_util_post_null_alloc_returns_error(void) {
    char *body = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_http_util_post(NULL, "https://example.com", 18, "", 0, NULL, 0, &body, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_http_util_post_null_url_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *body = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_http_util_post(&alloc, NULL, 18, "", 0, NULL, 0, &body, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_http_util_post_null_out_body_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 0;
    hu_error_t err = hu_http_util_post(&alloc, "https://x.com", 13, "", 0, NULL, 0, NULL, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_http_util_get_null_alloc_returns_error(void) {
    char *body = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_http_util_get(NULL, "https://example.com", 18, NULL, 0, NULL, &body, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_http_util_get_null_out_len_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *body = NULL;
    hu_error_t err = hu_http_util_get(&alloc, "https://x.com", 13, NULL, 0, NULL, &body, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

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
    HU_TEST_SUITE("util_modules (status, state, http_util, json_util, util)");

    HU_RUN_TEST(test_status_null_buf_returns_error);
    HU_RUN_TEST(test_status_small_buf_returns_error);
    HU_RUN_TEST(test_status_valid_buf_writes_output);
    HU_RUN_TEST(test_status_output_contains_version_or_no_config);

    HU_RUN_TEST(test_state_manager_init_deinit_lifecycle);
    HU_RUN_TEST(test_state_manager_init_null_mgr_returns_error);
    HU_RUN_TEST(test_state_manager_init_null_alloc_returns_error);
    HU_RUN_TEST(test_state_set_get_process);
    HU_RUN_TEST(test_state_get_process_null_returns_stopped);
    HU_RUN_TEST(test_state_set_get_last_channel);
    HU_RUN_TEST(test_state_default_path_returns_workspace_state_json);
    HU_RUN_TEST(test_state_default_path_null_alloc_returns_null);
    HU_RUN_TEST(test_state_default_path_null_workspace_returns_null);
    HU_RUN_TEST(test_state_save_load_roundtrip);

    HU_RUN_TEST(test_http_util_post_null_alloc_returns_error);
    HU_RUN_TEST(test_http_util_post_null_url_returns_error);
    HU_RUN_TEST(test_http_util_post_null_out_body_returns_error);
    HU_RUN_TEST(test_http_util_get_null_alloc_returns_error);
    HU_RUN_TEST(test_http_util_get_null_out_len_returns_error);

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
