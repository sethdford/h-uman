/* Auth module tests. Uses /tmp for credential file I/O; no network. */
#include "test_framework.h"
#include "seaclaw/auth.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

/* Setup writable auth dir: create /tmp/seaclaw_auth_test_<pid>_<n>/.seaclaw and set HOME. */
static char *setup_auth_test_home(sc_allocator_t *alloc) {
    static int counter = 0;
    char tmpdir[128];
    int n = snprintf(tmpdir, sizeof(tmpdir), "/tmp/seaclaw_auth_test_%d_%d", (int)getpid(), counter++);
    if (n <= 0 || (size_t)n >= sizeof(tmpdir)) return NULL;
    if (mkdir(tmpdir, 0755) != 0) return NULL;
    char subdir[256];
    n = snprintf(subdir, sizeof(subdir), "%s/.seaclaw", tmpdir);
    if (n <= 0 || (size_t)n >= sizeof(subdir)) { rmdir(tmpdir); return NULL; }
    if (mkdir(subdir, 0755) != 0) { rmdir(tmpdir); return NULL; }
    const char *old = getenv("HOME");
    char *saved = old ? sc_strdup(alloc, old) : NULL;
    setenv("HOME", tmpdir, 1);
    return saved;
}

static void restore_auth_test_home(sc_allocator_t *alloc, char *saved) {
    if (saved) {
        setenv("HOME", saved, 1);
        alloc->free(alloc->ctx, saved, strlen(saved) + 1);
    } else {
        unsetenv("HOME");
    }
}

/* ── OAuth token lifecycle ─────────────────────────────────────────────────── */

static void test_oauth_token_deinit_cleans_all(void) {
    sc_tracking_allocator_t *ta = sc_tracking_allocator_create();
    sc_allocator_t alloc = sc_tracking_allocator_allocator(ta);

    sc_oauth_token_t t = {0};
    t.access_token = sc_strdup(&alloc, "sk-test-token");
    t.refresh_token = sc_strdup(&alloc, "rt-refresh");
    t.token_type = sc_strdup(&alloc, "Bearer");
    t.expires_at = 3600;

    SC_ASSERT_EQ(sc_tracking_allocator_leaks(ta), 3u);
    sc_oauth_token_deinit(&t, &alloc);
    SC_ASSERT_EQ(sc_tracking_allocator_leaks(ta), 0u);
    SC_ASSERT_NULL(t.access_token);
    SC_ASSERT_NULL(t.refresh_token);
    SC_ASSERT_NULL(t.token_type);

    sc_tracking_allocator_destroy(ta);
}

static void test_oauth_token_deinit_null_safe(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_oauth_token_t t = {0};
    sc_oauth_token_deinit(&t, &sys);
    sc_oauth_token_deinit(NULL, &sys);
    sc_oauth_token_deinit(&t, NULL);
}

static void test_oauth_token_is_expired_null_safe(void) {
    SC_ASSERT_FALSE(sc_oauth_token_is_expired(NULL));
    sc_oauth_token_t t = { .expires_at = 0 };
    SC_ASSERT_FALSE(sc_oauth_token_is_expired(&t));
}

static void test_oauth_token_is_expired_future(void) {
    sc_oauth_token_t t = { .expires_at = (int64_t)time(NULL) + 3600 };
    SC_ASSERT_FALSE(sc_oauth_token_is_expired(&t));
}

/* ── API key round-trip (file I/O) ────────────────────────────────────────── */

static void test_auth_set_get_api_key_roundtrip(void) {
    sc_allocator_t sys = sc_system_allocator();
    char *saved = setup_auth_test_home(&sys);
    SC_ASSERT_NOT_NULL(saved);

    sc_error_t err = sc_auth_set_api_key(&sys, "openai", "sk-test-key-12345");
    SC_ASSERT_EQ(err, SC_OK);

    char *key = sc_auth_get_api_key(&sys, "openai");
    SC_ASSERT_NOT_NULL(key);
    SC_ASSERT_STR_EQ(key, "sk-test-key-12345");
    sys.free(sys.ctx, key, strlen(key) + 1);

    restore_auth_test_home(&sys, saved);
}

static void test_auth_get_api_key_nonexistent_provider(void) {
    sc_allocator_t sys = sc_system_allocator();
    char *saved = setup_auth_test_home(&sys);
    SC_ASSERT_NOT_NULL(saved);

    char *key = sc_auth_get_api_key(&sys, "nonexistent_provider_xyz");
    SC_ASSERT_NULL(key);

    restore_auth_test_home(&sys, saved);
}

static void test_auth_delete_credential(void) {
    sc_allocator_t sys = sc_system_allocator();
    char *saved = setup_auth_test_home(&sys);
    SC_ASSERT_NOT_NULL(saved);

    sc_error_t err = sc_auth_set_api_key(&sys, "anthropic", "sk-ant-key");
    SC_ASSERT_EQ(err, SC_OK);

    bool was_found = false;
    err = sc_auth_delete_credential(&sys, "anthropic", &was_found);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(was_found);

    char *key = sc_auth_get_api_key(&sys, "anthropic");
    SC_ASSERT_NULL(key);

    restore_auth_test_home(&sys, saved);
}

static void test_auth_delete_credential_nonexistent(void) {
    sc_allocator_t sys = sc_system_allocator();
    char *saved = setup_auth_test_home(&sys);
    SC_ASSERT_NOT_NULL(saved);

    bool was_found = true;
    sc_error_t err = sc_auth_delete_credential(&sys, "never_stored", &was_found);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_FALSE(was_found);

    restore_auth_test_home(&sys, saved);
}

/* ── Edge cases: NULL allocator, NULL provider, empty key ────────────────────── */

static void test_auth_set_api_key_null_allocator(void) {
    sc_error_t err = sc_auth_set_api_key(NULL, "openai", "sk-key");
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_auth_set_api_key_null_provider(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_error_t err = sc_auth_set_api_key(&sys, NULL, "sk-key");
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_auth_set_api_key_empty_key(void) {
    sc_allocator_t sys = sc_system_allocator();
    char *saved = setup_auth_test_home(&sys);
    SC_ASSERT_NOT_NULL(saved);

    sc_error_t err = sc_auth_set_api_key(&sys, "openai", "");
    SC_ASSERT_EQ(err, SC_OK);

    char *key = sc_auth_get_api_key(&sys, "openai");
    if (key) {
        SC_ASSERT_STR_EQ(key, "");
        sys.free(sys.ctx, key, 1);
    }

    restore_auth_test_home(&sys, saved);
}

static void test_auth_get_api_key_null_allocator(void) {
    char *key = sc_auth_get_api_key(NULL, "openai");
    SC_ASSERT_NULL(key);
}

static void test_auth_get_api_key_null_provider(void) {
    sc_allocator_t sys = sc_system_allocator();
    char *key = sc_auth_get_api_key(&sys, NULL);
    SC_ASSERT_NULL(key);
}

static void test_auth_save_credential_invalid_args(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_oauth_token_t tok = { .access_token = (char *)"x", .token_type = (char *)"Bearer" };
    SC_ASSERT_EQ(sc_auth_save_credential(NULL, "p", &tok), SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_EQ(sc_auth_save_credential(&sys, NULL, &tok), SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_EQ(sc_auth_save_credential(&sys, "p", NULL), SC_ERR_INVALID_ARGUMENT);
}

static void test_auth_load_credential_invalid_args(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_oauth_token_t tok = {0};
    SC_ASSERT_EQ(sc_auth_load_credential(NULL, "p", &tok), SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_EQ(sc_auth_load_credential(&sys, NULL, &tok), SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_EQ(sc_auth_load_credential(&sys, "p", NULL), SC_ERR_INVALID_ARGUMENT);
}

static void test_auth_delete_credential_invalid_args(void) {
    sc_allocator_t sys = sc_system_allocator();
    SC_ASSERT_EQ(sc_auth_delete_credential(NULL, "p", NULL), SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_EQ(sc_auth_delete_credential(&sys, NULL, NULL), SC_ERR_INVALID_ARGUMENT);
}

/* ── Device code flow (SC_IS_TEST returns mock, no network) ───────────────── */

static void test_auth_start_device_flow_mock(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_device_code_t dc = {0};
    sc_error_t err = sc_auth_start_device_flow(&sys, "client-id", "https://auth.example.com/device", "openid", &dc);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(dc.device_code);
    SC_ASSERT_NOT_NULL(dc.user_code);
    SC_ASSERT_NOT_NULL(dc.verification_uri);
    SC_ASSERT_STR_EQ(dc.device_code, "mock-device-code");
    SC_ASSERT_STR_EQ(dc.user_code, "MOCK-1234");
    SC_ASSERT_STR_EQ(dc.verification_uri, "https://example.com/activate");
    sc_device_code_deinit(&dc, &sys);
}

static void test_auth_start_device_flow_invalid_args(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_device_code_t dc = {0};
    SC_ASSERT_EQ(sc_auth_start_device_flow(NULL, "c", "u", "s", &dc), SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_EQ(sc_auth_start_device_flow(&sys, "c", "u", "s", NULL), SC_ERR_INVALID_ARGUMENT);
}

static void test_auth_poll_device_code_mock(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_oauth_token_t tok = {0};
    sc_error_t err = sc_auth_poll_device_code(&sys, "https://token.example.com", "client-id",
                                              "mock-device-code", 1, &tok);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(tok.access_token);
    SC_ASSERT_NOT_NULL(tok.refresh_token);
    SC_ASSERT_STR_EQ(tok.access_token, "mock-access-token");
    sc_oauth_token_deinit(&tok, &sys);
}

static void test_auth_poll_device_code_invalid_args(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_oauth_token_t tok = {0};
    SC_ASSERT_EQ(sc_auth_poll_device_code(NULL, "u", "c", "dc", 1, &tok), SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_EQ(sc_auth_poll_device_code(&sys, "u", "c", "dc", 1, NULL), SC_ERR_INVALID_ARGUMENT);
}

/* ── Token refresh (SC_IS_TEST returns mock) ────────────────────────────────── */

static void test_auth_refresh_token_mock(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_oauth_token_t tok = {0};
    sc_error_t err = sc_auth_refresh_token(&sys, "https://token.example.com", "client-id",
                                           "refresh-token-123", &tok);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(tok.access_token);
    SC_ASSERT_STR_EQ(tok.access_token, "mock-refreshed-token");
    sc_oauth_token_deinit(&tok, &sys);
}

static void test_auth_refresh_token_invalid_args(void) {
    sc_allocator_t sys = sc_system_allocator();
    sc_oauth_token_t tok = {0};
    SC_ASSERT_EQ(sc_auth_refresh_token(NULL, "u", "c", "rt", &tok), SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_EQ(sc_auth_refresh_token(&sys, "u", "c", "rt", NULL), SC_ERR_INVALID_ARGUMENT);
}

static void test_device_code_deinit_null_safe(void) {
    sc_device_code_t dc = {0};
    sc_allocator_t sys = sc_system_allocator();
    sc_device_code_deinit(&dc, &sys);
    sc_device_code_deinit(NULL, &sys);
    sc_device_code_deinit(&dc, NULL);
}

/* ── Full credential save/load (OAuth token, not just API key) ──────────────── */

static void test_auth_save_load_credential_roundtrip(void) {
    sc_allocator_t sys = sc_system_allocator();
    char *saved = setup_auth_test_home(&sys);
    SC_ASSERT_NOT_NULL(saved);

    sc_oauth_token_t in = {0};
    in.access_token = sc_strdup(&sys, "access-xyz");
    in.refresh_token = sc_strdup(&sys, "refresh-abc");
    in.token_type = sc_strdup(&sys, "Bearer");
    in.expires_at = time(NULL) + 7200;

    sc_error_t err = sc_auth_save_credential(&sys, "provider_x", &in);
    sc_oauth_token_deinit(&in, &sys);
    SC_ASSERT_EQ(err, SC_OK);

    sc_oauth_token_t out = {0};
    err = sc_auth_load_credential(&sys, "provider_x", &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out.access_token);
    SC_ASSERT_NOT_NULL(out.refresh_token);
    SC_ASSERT_STR_EQ(out.access_token, "access-xyz");
    SC_ASSERT_STR_EQ(out.refresh_token, "refresh-abc");
    sc_oauth_token_deinit(&out, &sys);

    restore_auth_test_home(&sys, saved);
}

void run_auth_tests(void) {
    SC_TEST_SUITE("Auth — OAuth token lifecycle");
    SC_RUN_TEST(test_oauth_token_deinit_cleans_all);
    SC_RUN_TEST(test_oauth_token_deinit_null_safe);
    SC_RUN_TEST(test_oauth_token_is_expired_null_safe);
    SC_RUN_TEST(test_oauth_token_is_expired_future);

    SC_TEST_SUITE("Auth — API key round-trip");
    SC_RUN_TEST(test_auth_set_get_api_key_roundtrip);
    SC_RUN_TEST(test_auth_get_api_key_nonexistent_provider);
    SC_RUN_TEST(test_auth_delete_credential);
    SC_RUN_TEST(test_auth_delete_credential_nonexistent);

    SC_TEST_SUITE("Auth — Credential save/load");
    SC_RUN_TEST(test_auth_save_load_credential_roundtrip);

    SC_TEST_SUITE("Auth — Edge cases");
    SC_RUN_TEST(test_auth_set_api_key_null_allocator);
    SC_RUN_TEST(test_auth_set_api_key_null_provider);
    SC_RUN_TEST(test_auth_set_api_key_empty_key);
    SC_RUN_TEST(test_auth_get_api_key_null_allocator);
    SC_RUN_TEST(test_auth_get_api_key_null_provider);
    SC_RUN_TEST(test_auth_save_credential_invalid_args);
    SC_RUN_TEST(test_auth_load_credential_invalid_args);
    SC_RUN_TEST(test_auth_delete_credential_invalid_args);

    SC_TEST_SUITE("Auth — Device flow (mock)");
    SC_RUN_TEST(test_auth_start_device_flow_mock);
    SC_RUN_TEST(test_auth_start_device_flow_invalid_args);
    SC_RUN_TEST(test_auth_poll_device_code_mock);
    SC_RUN_TEST(test_auth_poll_device_code_invalid_args);
    SC_RUN_TEST(test_device_code_deinit_null_safe);

    SC_TEST_SUITE("Auth — Token refresh (mock)");
    SC_RUN_TEST(test_auth_refresh_token_mock);
    SC_RUN_TEST(test_auth_refresh_token_invalid_args);
}
