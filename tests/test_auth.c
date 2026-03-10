/* Auth module tests. Uses /tmp for credential file I/O; no network. */
#include "human/auth.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Setup writable auth dir: create /tmp/human_auth_test_<pid>_<n>/.human and set HOME. */
static char *setup_auth_test_home(hu_allocator_t *alloc) {
    static int counter = 0;
    char tmpdir[128];
    int n =
        snprintf(tmpdir, sizeof(tmpdir), "/tmp/human_auth_test_%d_%d", (int)getpid(), counter++);
    if (n <= 0 || (size_t)n >= sizeof(tmpdir))
        return NULL;
    if (mkdir(tmpdir, 0755) != 0)
        return NULL;
    char subdir[256];
    n = snprintf(subdir, sizeof(subdir), "%s/.human", tmpdir);
    if (n <= 0 || (size_t)n >= sizeof(subdir)) {
        rmdir(tmpdir);
        return NULL;
    }
    if (mkdir(subdir, 0755) != 0) {
        rmdir(tmpdir);
        return NULL;
    }
    const char *old = getenv("HOME");
    char *saved = old ? hu_strdup(alloc, old) : NULL;
    setenv("HOME", tmpdir, 1);
    return saved;
}

static void restore_auth_test_home(hu_allocator_t *alloc, char *saved) {
    if (saved) {
        setenv("HOME", saved, 1);
        alloc->free(alloc->ctx, saved, strlen(saved) + 1);
    } else {
        unsetenv("HOME");
    }
}

/* ── OAuth token lifecycle ─────────────────────────────────────────────────── */

static void test_oauth_token_deinit_cleans_all(void) {
    hu_tracking_allocator_t *ta = hu_tracking_allocator_create();
    hu_allocator_t alloc = hu_tracking_allocator_allocator(ta);

    hu_oauth_token_t t = {0};
    t.access_token = hu_strdup(&alloc, "sk-test-token");
    t.refresh_token = hu_strdup(&alloc, "rt-refresh");
    t.token_type = hu_strdup(&alloc, "Bearer");
    t.expires_at = 3600;

    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 3u);
    hu_oauth_token_deinit(&t, &alloc);
    HU_ASSERT_EQ(hu_tracking_allocator_leaks(ta), 0u);
    HU_ASSERT_NULL(t.access_token);
    HU_ASSERT_NULL(t.refresh_token);
    HU_ASSERT_NULL(t.token_type);

    hu_tracking_allocator_destroy(ta);
}

static void test_oauth_token_deinit_null_safe(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_oauth_token_t t = {0};
    hu_oauth_token_deinit(&t, &sys);
    hu_oauth_token_deinit(NULL, &sys);
    hu_oauth_token_deinit(&t, NULL);
}

static void test_oauth_token_is_expired_null_safe(void) {
    HU_ASSERT_FALSE(hu_oauth_token_is_expired(NULL));
    hu_oauth_token_t t = {.expires_at = 0};
    HU_ASSERT_FALSE(hu_oauth_token_is_expired(&t));
}

static void test_oauth_token_is_expired_future(void) {
    hu_oauth_token_t t = {.expires_at = (int64_t)time(NULL) + 3600};
    HU_ASSERT_FALSE(hu_oauth_token_is_expired(&t));
}

/* ── API key round-trip (file I/O) ────────────────────────────────────────── */

static void test_auth_set_get_api_key_roundtrip(void) {
    hu_allocator_t sys = hu_system_allocator();
    char *saved = setup_auth_test_home(&sys);
    HU_ASSERT_NOT_NULL(saved);

    hu_error_t err = hu_auth_set_api_key(&sys, "openai", "sk-test-key-12345");
    HU_ASSERT_EQ(err, HU_OK);

    char *key = hu_auth_get_api_key(&sys, "openai");
    HU_ASSERT_NOT_NULL(key);
    HU_ASSERT_STR_EQ(key, "sk-test-key-12345");
    sys.free(sys.ctx, key, strlen(key) + 1);

    restore_auth_test_home(&sys, saved);
}

static void test_auth_get_api_key_nonexistent_provider(void) {
    hu_allocator_t sys = hu_system_allocator();
    char *saved = setup_auth_test_home(&sys);
    HU_ASSERT_NOT_NULL(saved);

    char *key = hu_auth_get_api_key(&sys, "nonexistent_provider_xyz");
    HU_ASSERT_NULL(key);

    restore_auth_test_home(&sys, saved);
}

static void test_auth_delete_credential(void) {
    hu_allocator_t sys = hu_system_allocator();
    char *saved = setup_auth_test_home(&sys);
    HU_ASSERT_NOT_NULL(saved);

    hu_error_t err = hu_auth_set_api_key(&sys, "anthropic", "sk-ant-key");
    HU_ASSERT_EQ(err, HU_OK);

    bool was_found = false;
    err = hu_auth_delete_credential(&sys, "anthropic", &was_found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(was_found);

    char *key = hu_auth_get_api_key(&sys, "anthropic");
    HU_ASSERT_NULL(key);

    restore_auth_test_home(&sys, saved);
}

static void test_auth_delete_credential_nonexistent(void) {
    hu_allocator_t sys = hu_system_allocator();
    char *saved = setup_auth_test_home(&sys);
    HU_ASSERT_NOT_NULL(saved);

    bool was_found = true;
    hu_error_t err = hu_auth_delete_credential(&sys, "never_stored", &was_found);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_FALSE(was_found);

    restore_auth_test_home(&sys, saved);
}

/* ── Edge cases: NULL allocator, NULL provider, empty key ────────────────────── */

static void test_auth_set_api_key_null_allocator(void) {
    hu_error_t err = hu_auth_set_api_key(NULL, "openai", "sk-key");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_auth_set_api_key_null_provider(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_error_t err = hu_auth_set_api_key(&sys, NULL, "sk-key");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_auth_set_api_key_empty_key(void) {
    hu_allocator_t sys = hu_system_allocator();
    char *saved = setup_auth_test_home(&sys);
    HU_ASSERT_NOT_NULL(saved);

    hu_error_t err = hu_auth_set_api_key(&sys, "openai", "");
    HU_ASSERT_EQ(err, HU_OK);

    char *key = hu_auth_get_api_key(&sys, "openai");
    if (key) {
        HU_ASSERT_STR_EQ(key, "");
        sys.free(sys.ctx, key, 1);
    }

    restore_auth_test_home(&sys, saved);
}

static void test_auth_get_api_key_null_allocator(void) {
    char *key = hu_auth_get_api_key(NULL, "openai");
    HU_ASSERT_NULL(key);
}

static void test_auth_get_api_key_null_provider(void) {
    hu_allocator_t sys = hu_system_allocator();
    char *key = hu_auth_get_api_key(&sys, NULL);
    HU_ASSERT_NULL(key);
}

static void test_auth_save_credential_invalid_args(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_oauth_token_t tok = {.access_token = (char *)"x", .token_type = (char *)"Bearer"};
    HU_ASSERT_EQ(hu_auth_save_credential(NULL, "p", &tok), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_auth_save_credential(&sys, NULL, &tok), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_auth_save_credential(&sys, "p", NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_auth_load_credential_invalid_args(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_oauth_token_t tok = {0};
    HU_ASSERT_EQ(hu_auth_load_credential(NULL, "p", &tok), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_auth_load_credential(&sys, NULL, &tok), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_auth_load_credential(&sys, "p", NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_auth_delete_credential_invalid_args(void) {
    hu_allocator_t sys = hu_system_allocator();
    HU_ASSERT_EQ(hu_auth_delete_credential(NULL, "p", NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_auth_delete_credential(&sys, NULL, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* ── Device code flow (HU_IS_TEST returns mock, no network) ───────────────── */

static void test_auth_start_device_flow_mock(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_device_code_t dc = {0};
    hu_error_t err = hu_auth_start_device_flow(&sys, "client-id", "https://auth.example.com/device",
                                               "openid", &dc);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(dc.device_code);
    HU_ASSERT_NOT_NULL(dc.user_code);
    HU_ASSERT_NOT_NULL(dc.verification_uri);
    HU_ASSERT_STR_EQ(dc.device_code, "mock-device-code");
    HU_ASSERT_STR_EQ(dc.user_code, "MOCK-1234");
    HU_ASSERT_STR_EQ(dc.verification_uri, "https://example.com/activate");
    hu_device_code_deinit(&dc, &sys);
}

static void test_auth_start_device_flow_invalid_args(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_device_code_t dc = {0};
    HU_ASSERT_EQ(hu_auth_start_device_flow(NULL, "c", "u", "s", &dc), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_auth_start_device_flow(&sys, "c", "u", "s", NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_auth_poll_device_code_mock(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_oauth_token_t tok = {0};
    hu_error_t err = hu_auth_poll_device_code(&sys, "https://token.example.com", "client-id",
                                              "mock-device-code", 1, &tok);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tok.access_token);
    HU_ASSERT_NOT_NULL(tok.refresh_token);
    HU_ASSERT_STR_EQ(tok.access_token, "mock-access-token");
    hu_oauth_token_deinit(&tok, &sys);
}

static void test_auth_poll_device_code_invalid_args(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_oauth_token_t tok = {0};
    HU_ASSERT_EQ(hu_auth_poll_device_code(NULL, "u", "c", "dc", 1, &tok), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_auth_poll_device_code(&sys, "u", "c", "dc", 1, NULL), HU_ERR_INVALID_ARGUMENT);
}

/* ── Token refresh (HU_IS_TEST returns mock) ────────────────────────────────── */

static void test_auth_refresh_token_mock(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_oauth_token_t tok = {0};
    hu_error_t err = hu_auth_refresh_token(&sys, "https://token.example.com", "client-id",
                                           "refresh-token-123", &tok);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tok.access_token);
    HU_ASSERT_STR_EQ(tok.access_token, "mock-refreshed-token");
    hu_oauth_token_deinit(&tok, &sys);
}

static void test_auth_refresh_token_invalid_args(void) {
    hu_allocator_t sys = hu_system_allocator();
    hu_oauth_token_t tok = {0};
    HU_ASSERT_EQ(hu_auth_refresh_token(NULL, "u", "c", "rt", &tok), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_auth_refresh_token(&sys, "u", "c", "rt", NULL), HU_ERR_INVALID_ARGUMENT);
}

static void test_device_code_deinit_null_safe(void) {
    hu_device_code_t dc = {0};
    hu_allocator_t sys = hu_system_allocator();
    hu_device_code_deinit(&dc, &sys);
    hu_device_code_deinit(NULL, &sys);
    hu_device_code_deinit(&dc, NULL);
}

/* ── Full credential save/load (OAuth token, not just API key) ──────────────── */

static void test_auth_save_load_credential_roundtrip(void) {
    hu_allocator_t sys = hu_system_allocator();
    char *saved = setup_auth_test_home(&sys);
    HU_ASSERT_NOT_NULL(saved);

    hu_oauth_token_t in = {0};
    in.access_token = hu_strdup(&sys, "access-xyz");
    in.refresh_token = hu_strdup(&sys, "refresh-abc");
    in.token_type = hu_strdup(&sys, "Bearer");
    in.expires_at = time(NULL) + 7200;

    hu_error_t err = hu_auth_save_credential(&sys, "provider_x", &in);
    hu_oauth_token_deinit(&in, &sys);
    HU_ASSERT_EQ(err, HU_OK);

    hu_oauth_token_t out = {0};
    err = hu_auth_load_credential(&sys, "provider_x", &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out.access_token);
    HU_ASSERT_NOT_NULL(out.refresh_token);
    HU_ASSERT_STR_EQ(out.access_token, "access-xyz");
    HU_ASSERT_STR_EQ(out.refresh_token, "refresh-abc");
    hu_oauth_token_deinit(&out, &sys);

    restore_auth_test_home(&sys, saved);
}

void run_auth_tests(void) {
    HU_TEST_SUITE("Auth — OAuth token lifecycle");
    HU_RUN_TEST(test_oauth_token_deinit_cleans_all);
    HU_RUN_TEST(test_oauth_token_deinit_null_safe);
    HU_RUN_TEST(test_oauth_token_is_expired_null_safe);
    HU_RUN_TEST(test_oauth_token_is_expired_future);

    HU_TEST_SUITE("Auth — API key round-trip");
    HU_RUN_TEST(test_auth_set_get_api_key_roundtrip);
    HU_RUN_TEST(test_auth_get_api_key_nonexistent_provider);
    HU_RUN_TEST(test_auth_delete_credential);
    HU_RUN_TEST(test_auth_delete_credential_nonexistent);

    HU_TEST_SUITE("Auth — Credential save/load");
    HU_RUN_TEST(test_auth_save_load_credential_roundtrip);

    HU_TEST_SUITE("Auth — Edge cases");
    HU_RUN_TEST(test_auth_set_api_key_null_allocator);
    HU_RUN_TEST(test_auth_set_api_key_null_provider);
    HU_RUN_TEST(test_auth_set_api_key_empty_key);
    HU_RUN_TEST(test_auth_get_api_key_null_allocator);
    HU_RUN_TEST(test_auth_get_api_key_null_provider);
    HU_RUN_TEST(test_auth_save_credential_invalid_args);
    HU_RUN_TEST(test_auth_load_credential_invalid_args);
    HU_RUN_TEST(test_auth_delete_credential_invalid_args);

    HU_TEST_SUITE("Auth — Device flow (mock)");
    HU_RUN_TEST(test_auth_start_device_flow_mock);
    HU_RUN_TEST(test_auth_start_device_flow_invalid_args);
    HU_RUN_TEST(test_auth_poll_device_code_mock);
    HU_RUN_TEST(test_auth_poll_device_code_invalid_args);
    HU_RUN_TEST(test_device_code_deinit_null_safe);

    HU_TEST_SUITE("Auth — Token refresh (mock)");
    HU_RUN_TEST(test_auth_refresh_token_mock);
    HU_RUN_TEST(test_auth_refresh_token_invalid_args);
}
