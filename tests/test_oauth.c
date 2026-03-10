#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/gateway/oauth.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

static void test_oauth_init_and_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_oauth_config_t cfg = {.provider = "google",
                             .client_id = "cid",
                             .client_secret = "cs",
                             .redirect_uri = "https://example.com/cb"};
    hu_oauth_ctx_t *ctx = NULL;
    hu_error_t err = hu_oauth_init(&alloc, &cfg, &ctx);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    hu_oauth_destroy(ctx);
}

static void test_oauth_init_null_fails(void) {
    hu_error_t err = hu_oauth_init(NULL, NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_oauth_pkce_generation(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_oauth_config_t cfg = {.provider = "google"};
    hu_oauth_ctx_t *ctx = NULL;
    hu_oauth_init(&alloc, &cfg, &ctx);
    char verifier[64] = {0};
    char challenge[64] = {0};
    hu_error_t err =
        hu_oauth_generate_pkce(ctx, verifier, sizeof(verifier), challenge, sizeof(challenge));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strlen(verifier) == 43);
    HU_ASSERT_TRUE(strlen(challenge) > 0);
    hu_oauth_destroy(ctx);
}

static void test_oauth_build_auth_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_oauth_config_t cfg = {.provider = "google",
                             .client_id = "test-id",
                             .redirect_uri = "https://localhost:3000/auth/callback",
                             .scopes = "openid email"};
    hu_oauth_ctx_t *ctx = NULL;
    hu_oauth_init(&alloc, &cfg, &ctx);
    char url[1024] = {0};
    hu_error_t err = hu_oauth_build_auth_url(ctx, "challenge", 9, "state123", 8, url, sizeof(url));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(url, "accounts.google.com") != NULL);
    HU_ASSERT_TRUE(strstr(url, "test-id") != NULL);
    HU_ASSERT_TRUE(strstr(url, "challenge") != NULL);
    HU_ASSERT_TRUE(strstr(url, "S256") != NULL);
    hu_oauth_destroy(ctx);
}

static void test_oauth_exchange_code_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_oauth_config_t cfg = {.provider = "google",
                             .client_id = "cid",
                             .client_secret = "cs",
                             .redirect_uri = "https://example.com/cb"};
    hu_oauth_ctx_t *ctx = NULL;
    hu_oauth_init(&alloc, &cfg, &ctx);
    hu_oauth_session_t session = {0};
    hu_error_t err = hu_oauth_exchange_code(ctx, "test-code", 9, "test-verifier", 13, &session);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strlen(session.session_id) > 0);
    HU_ASSERT_TRUE(strlen(session.access_token) > 0);
    HU_ASSERT_TRUE(hu_oauth_session_valid(&session));
    hu_oauth_destroy(ctx);
}

static void test_oauth_refresh_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_oauth_config_t cfg = {.provider = "google"};
    hu_oauth_ctx_t *ctx = NULL;
    hu_oauth_init(&alloc, &cfg, &ctx);
    hu_oauth_session_t session = {0};
    snprintf(session.session_id, sizeof(session.session_id), "test-sess");
    snprintf(session.access_token, sizeof(session.access_token), "old-token");
    session.expires_at = (int64_t)time(NULL) - 100;
    HU_ASSERT_FALSE(hu_oauth_session_valid(&session));
    hu_error_t err = hu_oauth_refresh_token(ctx, &session);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(hu_oauth_session_valid(&session));
    hu_oauth_destroy(ctx);
}

static void test_oauth_session_expired(void) {
    hu_oauth_session_t session = {0};
    snprintf(session.session_id, sizeof(session.session_id), "expired");
    session.expires_at = (int64_t)time(NULL) - 1;
    HU_ASSERT_FALSE(hu_oauth_session_valid(&session));
}

static void test_oauth_session_empty_invalid(void) {
    hu_oauth_session_t session = {0};
    HU_ASSERT_FALSE(hu_oauth_session_valid(&session));
    HU_ASSERT_FALSE(hu_oauth_session_valid(NULL));
}

void run_oauth_tests(void) {
    HU_RUN_TEST(test_oauth_init_and_destroy);
    HU_RUN_TEST(test_oauth_init_null_fails);
    HU_RUN_TEST(test_oauth_pkce_generation);
    HU_RUN_TEST(test_oauth_build_auth_url);
    HU_RUN_TEST(test_oauth_exchange_code_mock);
    HU_RUN_TEST(test_oauth_refresh_mock);
    HU_RUN_TEST(test_oauth_session_expired);
    HU_RUN_TEST(test_oauth_session_empty_invalid);
}
