/* Tests for status.c, state.c, http_util.c, json_util.c, util.c — no network, minimal file I/O (/tmp only). */
#include "test_framework.h"
#include "seaclaw/status.h"
#include "seaclaw/state.h"
#include "seaclaw/http_util.h"
#include "seaclaw/json_util.h"
#include "seaclaw/util.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/error.h"
#include <string.h>
#include <unistd.h>

/* ─── status.c ─────────────────────────────────────────────────────────────── */

static void test_status_null_buf_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_status_run(&alloc, NULL, 256);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_status_small_buf_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[64];
    sc_error_t err = sc_status_run(&alloc, buf, sizeof(buf));
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_status_valid_buf_writes_output(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[512] = {0};
    sc_error_t err = sc_status_run(&alloc, buf, sizeof(buf));
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(strstr(buf, "SeaClaw"));
    SC_ASSERT_NOT_NULL(strstr(buf, "Version"));
}

static void test_status_output_contains_version_or_no_config(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char buf[512] = {0};
    sc_status_run(&alloc, buf, sizeof(buf));
    int has_version = (strstr(buf, "Version") != NULL);
    int has_no_config = (strstr(buf, "no config") != NULL);
    SC_ASSERT_TRUE(has_version || has_no_config);
}

/* ─── state.c ─────────────────────────────────────────────────────────────── */

static void test_state_manager_init_deinit_lifecycle(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_state_manager_t mgr;
    SC_ASSERT_EQ(sc_state_manager_init(&mgr, &alloc, NULL), SC_OK);
    SC_ASSERT_EQ(mgr.process_state, SC_PROCESS_STATE_STARTING);
    SC_ASSERT_NULL(mgr.state_path);
    sc_state_manager_deinit(&mgr);
}

static void test_state_manager_init_null_mgr_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_state_manager_init(NULL, &alloc, "/tmp/state.json");
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_state_manager_init_null_alloc_returns_error(void) {
    sc_state_manager_t mgr;
    sc_error_t err = sc_state_manager_init(&mgr, NULL, "/tmp/state.json");
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_state_set_get_process(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_state_manager_t mgr;
    sc_state_manager_init(&mgr, &alloc, NULL);

    sc_state_set_process(&mgr, SC_PROCESS_STATE_RUNNING);
    SC_ASSERT_EQ(sc_state_get_process(&mgr), SC_PROCESS_STATE_RUNNING);

    sc_state_set_process(&mgr, SC_PROCESS_STATE_STOPPING);
    SC_ASSERT_EQ(sc_state_get_process(&mgr), SC_PROCESS_STATE_STOPPING);

    sc_state_manager_deinit(&mgr);
}

static void test_state_get_process_null_returns_stopped(void) {
    SC_ASSERT_EQ(sc_state_get_process(NULL), SC_PROCESS_STATE_STOPPED);
}

static void test_state_set_get_last_channel(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_state_manager_t mgr;
    sc_state_manager_init(&mgr, &alloc, NULL);

    sc_state_set_last_channel(&mgr, "cli", "chat_abc");
    char ch[SC_STATE_CHANNEL_LEN], cid[SC_STATE_CHAT_ID_LEN];
    sc_state_get_last_channel(&mgr, ch, sizeof(ch), cid, sizeof(cid));
    SC_ASSERT_STR_EQ(ch, "cli");
    SC_ASSERT_STR_EQ(cid, "chat_abc");

    sc_state_manager_deinit(&mgr);
}

static void test_state_default_path_returns_workspace_state_json(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *p = sc_state_default_path(&alloc, "/tmp/workspace");
    SC_ASSERT_NOT_NULL(p);
    SC_ASSERT_NOT_NULL(strstr(p, "state.json"));
    SC_ASSERT_NOT_NULL(strstr(p, "/tmp/workspace"));
    alloc.free(alloc.ctx, p, strlen(p) + 1);
}

static void test_state_default_path_null_alloc_returns_null(void) {
    SC_ASSERT_NULL(sc_state_default_path(NULL, "/tmp/ws"));
}

static void test_state_default_path_null_workspace_returns_null(void) {
    sc_allocator_t alloc = sc_system_allocator();
    SC_ASSERT_NULL(sc_state_default_path(&alloc, NULL));
}

static void test_state_save_load_roundtrip(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char path_buf[256];
    snprintf(path_buf, sizeof(path_buf), "/tmp/seaclaw_state_test_%u.json", (unsigned)getpid());
    sc_state_manager_t mgr;
    sc_state_manager_init(&mgr, &alloc, path_buf);
    sc_state_set_last_channel(&mgr, "test_channel", "test_chat_id");

    SC_ASSERT_EQ(sc_state_save(&mgr), SC_OK);
    sc_state_set_last_channel(&mgr, "", "");
    SC_ASSERT_EQ(sc_state_load(&mgr), SC_OK);

    char ch[SC_STATE_CHANNEL_LEN], cid[SC_STATE_CHAT_ID_LEN];
    sc_state_get_last_channel(&mgr, ch, sizeof(ch), cid, sizeof(cid));
    SC_ASSERT_STR_EQ(ch, "test_channel");
    SC_ASSERT_STR_EQ(cid, "test_chat_id");

    sc_state_manager_deinit(&mgr);
    remove(path_buf);
}

/* ─── http_util.c ───────────────────────────────────────────────────────────
 * No network — test only argument validation. */

static void test_http_util_post_null_alloc_returns_error(void) {
    char *body = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_http_util_post(NULL, "https://example.com", 18,
        "", 0, NULL, 0, &body, &out_len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_http_util_post_null_url_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *body = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_http_util_post(&alloc, NULL, 18, "", 0, NULL, 0, &body, &out_len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_http_util_post_null_out_body_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    size_t out_len = 0;
    sc_error_t err = sc_http_util_post(&alloc, "https://x.com", 13, "", 0, NULL, 0, NULL, &out_len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_http_util_get_null_alloc_returns_error(void) {
    char *body = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_http_util_get(NULL, "https://example.com", 18,
        NULL, 0, NULL, &body, &out_len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_http_util_get_null_out_len_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *body = NULL;
    sc_error_t err = sc_http_util_get(&alloc, "https://x.com", 13, NULL, 0, NULL, &body, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

/* ─── json_util.c ─────────────────────────────────────────────────────────── */

static void test_json_util_append_string_null_buf_returns_error(void) {
    sc_error_t err = sc_json_util_append_string(NULL, "x");
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_json_util_append_string_null_s_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    sc_json_buf_init(&buf, &alloc);
    sc_error_t err = sc_json_util_append_string(&buf, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
    sc_json_buf_free(&buf);
}

static void test_json_util_append_string_success(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    SC_ASSERT_EQ(sc_json_buf_init(&buf, &alloc), SC_OK);
    SC_ASSERT_EQ(sc_json_util_append_string(&buf, "hello"), SC_OK);
    SC_ASSERT_TRUE(buf.len >= 5);
    SC_ASSERT_NOT_NULL(strstr(buf.ptr, "hello"));
    sc_json_buf_free(&buf);
}

static void test_json_util_append_key_null_key_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    sc_json_buf_init(&buf, &alloc);
    sc_error_t err = sc_json_util_append_key(&buf, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
    sc_json_buf_free(&buf);
}

static void test_json_util_append_key_value_success(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    SC_ASSERT_EQ(sc_json_buf_init(&buf, &alloc), SC_OK);
    SC_ASSERT_EQ(sc_json_util_append_key_value(&buf, "name", "Alice"), SC_OK);
    SC_ASSERT_TRUE(buf.len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf.ptr, "name"));
    SC_ASSERT_NOT_NULL(strstr(buf.ptr, "Alice"));
    sc_json_buf_free(&buf);
}

static void test_json_util_append_key_int_success(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_json_buf_t buf;
    SC_ASSERT_EQ(sc_json_buf_init(&buf, &alloc), SC_OK);
    SC_ASSERT_EQ(sc_json_util_append_key_int(&buf, "count", 42), SC_OK);
    SC_ASSERT_TRUE(buf.len > 0);
    SC_ASSERT_NOT_NULL(strstr(buf.ptr, "count"));
    SC_ASSERT_NOT_NULL(strstr(buf.ptr, "42"));
    sc_json_buf_free(&buf);
}

/* ─── util.c ──────────────────────────────────────────────────────────────── */

static void test_util_trim_empty_string(void) {
    char s[] = "   ";
    size_t n = sc_util_trim(s, 3);
    SC_ASSERT_EQ(n, 0);
    SC_ASSERT_EQ(s[0], '\0');
}

static void test_util_trim_leading_trailing(void) {
    char s[] = "  hello  ";
    size_t n = sc_util_trim(s, 9);
    SC_ASSERT_EQ(n, 5);
    SC_ASSERT_STR_EQ(s, "hello");
}

static void test_util_trim_no_whitespace(void) {
    char s[] = "hello";
    size_t n = sc_util_trim(s, 5);
    SC_ASSERT_EQ(n, 5);
    SC_ASSERT_STR_EQ(s, "hello");
}

static void test_util_trim_null_returns_zero(void) {
    SC_ASSERT_EQ(sc_util_trim(NULL, 10), 0);
}

static void test_util_trim_zero_len_returns_zero(void) {
    char s[] = "  x  ";
    SC_ASSERT_EQ(sc_util_trim(s, 0), 0);
}

static void test_util_strdup_success(void) {
    sc_allocator_t alloc = sc_system_allocator();
    /* alloc.ctx may be NULL for system allocator; use alloc ptr as non-null ctx */
    void *ctx = alloc.ctx ? alloc.ctx : (void *)&alloc;
    char *dup = sc_util_strdup(ctx, alloc.alloc, "test_string");
    SC_ASSERT_NOT_NULL(dup);
    SC_ASSERT_STR_EQ(dup, "test_string");
    sc_util_strfree(ctx, alloc.free, dup);
}

static void test_util_strdup_null_s_returns_null(void) {
    sc_allocator_t alloc = sc_system_allocator();
    SC_ASSERT_NULL(sc_util_strdup(alloc.ctx, alloc.alloc, NULL));
}

static void test_util_strdup_null_alloc_returns_null(void) {
    SC_ASSERT_NULL(sc_util_strdup(NULL, NULL, "x"));
}

static void test_util_strcasecmp_equal(void) {
    SC_ASSERT_EQ(sc_util_strcasecmp("Hello", "hello"), 0);
    SC_ASSERT_EQ(sc_util_strcasecmp("ABC", "abc"), 0);
}

static void test_util_strcasecmp_less_greater(void) {
    SC_ASSERT_TRUE(sc_util_strcasecmp("a", "b") < 0);
    SC_ASSERT_TRUE(sc_util_strcasecmp("b", "a") > 0);
}

static void test_util_strcasecmp_null_a(void) {
    SC_ASSERT_TRUE(sc_util_strcasecmp(NULL, "x") < 0);
}

static void test_util_strcasecmp_null_b(void) {
    SC_ASSERT_TRUE(sc_util_strcasecmp("x", NULL) > 0);
}

static void test_util_strcasecmp_both_null(void) {
    SC_ASSERT_EQ(sc_util_strcasecmp(NULL, NULL), 0);
}

static void test_util_gen_session_id_returns_non_null(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *sid = sc_util_gen_session_id(alloc.ctx, alloc.alloc);
    SC_ASSERT_NOT_NULL(sid);
    SC_ASSERT_TRUE(strlen(sid) > 0);
    alloc.free(alloc.ctx, sid, strlen(sid) + 1);
}

static void test_util_gen_session_id_null_alloc_returns_null(void) {
    SC_ASSERT_NULL(sc_util_gen_session_id(NULL, NULL));
}

/* ─── suite ───────────────────────────────────────────────────────────────── */

void run_util_modules_tests(void) {
    SC_TEST_SUITE("util_modules (status, state, http_util, json_util, util)");

    SC_RUN_TEST(test_status_null_buf_returns_error);
    SC_RUN_TEST(test_status_small_buf_returns_error);
    SC_RUN_TEST(test_status_valid_buf_writes_output);
    SC_RUN_TEST(test_status_output_contains_version_or_no_config);

    SC_RUN_TEST(test_state_manager_init_deinit_lifecycle);
    SC_RUN_TEST(test_state_manager_init_null_mgr_returns_error);
    SC_RUN_TEST(test_state_manager_init_null_alloc_returns_error);
    SC_RUN_TEST(test_state_set_get_process);
    SC_RUN_TEST(test_state_get_process_null_returns_stopped);
    SC_RUN_TEST(test_state_set_get_last_channel);
    SC_RUN_TEST(test_state_default_path_returns_workspace_state_json);
    SC_RUN_TEST(test_state_default_path_null_alloc_returns_null);
    SC_RUN_TEST(test_state_default_path_null_workspace_returns_null);
    SC_RUN_TEST(test_state_save_load_roundtrip);

    SC_RUN_TEST(test_http_util_post_null_alloc_returns_error);
    SC_RUN_TEST(test_http_util_post_null_url_returns_error);
    SC_RUN_TEST(test_http_util_post_null_out_body_returns_error);
    SC_RUN_TEST(test_http_util_get_null_alloc_returns_error);
    SC_RUN_TEST(test_http_util_get_null_out_len_returns_error);

    SC_RUN_TEST(test_json_util_append_string_null_buf_returns_error);
    SC_RUN_TEST(test_json_util_append_string_null_s_returns_error);
    SC_RUN_TEST(test_json_util_append_string_success);
    SC_RUN_TEST(test_json_util_append_key_null_key_returns_error);
    SC_RUN_TEST(test_json_util_append_key_value_success);
    SC_RUN_TEST(test_json_util_append_key_int_success);

    SC_RUN_TEST(test_util_trim_empty_string);
    SC_RUN_TEST(test_util_trim_leading_trailing);
    SC_RUN_TEST(test_util_trim_no_whitespace);
    SC_RUN_TEST(test_util_trim_null_returns_zero);
    SC_RUN_TEST(test_util_trim_zero_len_returns_zero);
    SC_RUN_TEST(test_util_strdup_success);
    SC_RUN_TEST(test_util_strdup_null_s_returns_null);
    SC_RUN_TEST(test_util_strdup_null_alloc_returns_null);
    SC_RUN_TEST(test_util_strcasecmp_equal);
    SC_RUN_TEST(test_util_strcasecmp_less_greater);
    SC_RUN_TEST(test_util_strcasecmp_null_a);
    SC_RUN_TEST(test_util_strcasecmp_null_b);
    SC_RUN_TEST(test_util_strcasecmp_both_null);
    SC_RUN_TEST(test_util_gen_session_id_returns_non_null);
    SC_RUN_TEST(test_util_gen_session_id_null_alloc_returns_null);
}
