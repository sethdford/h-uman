#include "human/core/allocator.h"
#include "human/tunnel.h"
#include "test_framework.h"
#include <string.h>

static void test_none_tunnel_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_none_tunnel_create(&alloc);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_NOT_NULL(t.vtable);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "none");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_none_tunnel_start_returns_localhost(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_none_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    hu_tunnel_error_t err = t.vtable->start(t.ctx, 8080, &url, &url_len);
    HU_ASSERT_EQ(err, HU_TUNNEL_ERR_OK);
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(strstr(url, "localhost") != NULL);
    HU_ASSERT_TRUE(strstr(url, "8080") != NULL);
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_config_parsing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_config_t config = {
        .provider = HU_TUNNEL_NONE,
    };
    hu_tunnel_t t = hu_tunnel_create(&alloc, &config);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "none");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_create_null_config_uses_none(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_tunnel_create(&alloc, NULL);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "none");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_error_string(void) {
    HU_ASSERT_NOT_NULL(hu_tunnel_error_string(HU_TUNNEL_ERR_OK));
    HU_ASSERT_NOT_NULL(hu_tunnel_error_string(HU_TUNNEL_ERR_START_FAILED));
}

static void test_tailscale_tunnel_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_tailscale_tunnel_create(&alloc);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_NOT_NULL(t.vtable);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "tailscale");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tailscale_tunnel_start_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_tailscale_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    hu_tunnel_error_t err = t.vtable->start(t.ctx, 8080, &url, &url_len);
    HU_ASSERT_EQ(err, HU_TUNNEL_ERR_OK);
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(strstr(url, "ts.net") != NULL);
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_custom_tunnel_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *cmd = "echo https://example.com";
    hu_tunnel_t t = hu_custom_tunnel_create(&alloc, cmd, strlen(cmd));
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_NOT_NULL(t.vtable);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "custom");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_custom_tunnel_start_test_mode(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *cmd = "echo https://serveo.example";
    hu_tunnel_t t = hu_custom_tunnel_create(&alloc, cmd, strlen(cmd));
    char *url = NULL;
    size_t url_len = 0;
    hu_tunnel_error_t err = t.vtable->start(t.ctx, 3000, &url, &url_len);
    HU_ASSERT_EQ(err, HU_TUNNEL_ERR_OK);
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(strstr(url, "custom-mock") != NULL);
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_factory_tailscale(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_config_t config = {
        .provider = HU_TUNNEL_TAILSCALE,
    };
    hu_tunnel_t t = hu_tunnel_create(&alloc, &config);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "tailscale");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_factory_custom(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *cmd = "ssh -R 80:localhost:{port} serveo.net";
    hu_tunnel_config_t config = {
        .provider = HU_TUNNEL_CUSTOM,
        .custom_start_cmd = cmd,
        .custom_start_cmd_len = strlen(cmd),
    };
    hu_tunnel_t t = hu_tunnel_create(&alloc, &config);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "custom");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_factory_cloudflare(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_config_t config = {
        .provider = HU_TUNNEL_CLOUDFLARE,
        .cloudflare_token = "test-token",
        .cloudflare_token_len = 10,
    };
    hu_tunnel_t t = hu_tunnel_create(&alloc, &config);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "cloudflare");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_factory_ngrok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_config_t config = {
        .provider = HU_TUNNEL_NGROK,
        .ngrok_auth_token = "tok",
        .ngrok_auth_token_len = 3,
        .ngrok_domain = NULL,
        .ngrok_domain_len = 0,
    };
    hu_tunnel_t t = hu_tunnel_create(&alloc, &config);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "ngrok");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_none_is_running_after_start(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_none_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    t.vtable->start(t.ctx, 3000, &url, &url_len);
    HU_ASSERT_TRUE(t.vtable->is_running(t.ctx));
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_none_public_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_none_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    t.vtable->start(t.ctx, 8080, &url, &url_len);
    const char *pub = t.vtable->public_url(t.ctx);
    HU_ASSERT_NOT_NULL(pub);
    HU_ASSERT_TRUE(strstr(pub, "localhost") != NULL);
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_error_strings_all(void) {
    HU_ASSERT_NOT_NULL(hu_tunnel_error_string(HU_TUNNEL_ERR_OK));
    HU_ASSERT_NOT_NULL(hu_tunnel_error_string(HU_TUNNEL_ERR_PROCESS_SPAWN));
    HU_ASSERT_NOT_NULL(hu_tunnel_error_string(HU_TUNNEL_ERR_URL_NOT_FOUND));
    HU_ASSERT_NOT_NULL(hu_tunnel_error_string(HU_TUNNEL_ERR_TIMEOUT));
    HU_ASSERT_NOT_NULL(hu_tunnel_error_string(HU_TUNNEL_ERR_INVALID_COMMAND));
    HU_ASSERT_NOT_NULL(hu_tunnel_error_string(HU_TUNNEL_ERR_NOT_IMPLEMENTED));
}

static void test_tunnel_create_null_alloc_returns_invalid(void) {
    hu_tunnel_config_t config = {.provider = HU_TUNNEL_NONE};
    hu_tunnel_t t = hu_tunnel_create(NULL, &config);
    HU_ASSERT_NULL(t.ctx);
    HU_ASSERT_NULL(t.vtable);
}

static void test_tunnel_none_stop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_none_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    t.vtable->start(t.ctx, 8080, &url, &url_len);
    t.vtable->stop(t.ctx);
    HU_ASSERT_FALSE(t.vtable->is_running(t.ctx));
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_custom_url_contains_port_placeholder(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_custom_tunnel_create(&alloc, "echo https://example.com:{port}", 31);
    char *url = NULL;
    size_t url_len = 0;
    hu_tunnel_error_t err = t.vtable->start(t.ctx, 4567, &url, &url_len);
    HU_ASSERT_EQ(err, HU_TUNNEL_ERR_OK);
    HU_ASSERT_NOT_NULL(url);
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_tailscale_returns_url(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_tailscale_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    hu_tunnel_error_t err = t.vtable->start(t.ctx, 9000, &url, &url_len);
    HU_ASSERT_EQ(err, HU_TUNNEL_ERR_OK);
    HU_ASSERT_NOT_NULL(url);
    HU_ASSERT_TRUE(strstr(url, "ts.net") != NULL);
    HU_ASSERT_NOT_NULL(t.vtable->public_url(t.ctx));
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_config_ngrok_domain(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_config_t config = {
        .provider = HU_TUNNEL_NGROK,
        .ngrok_auth_token = "x",
        .ngrok_auth_token_len = 1,
        .ngrok_domain = "myapp.ngrok.io",
        .ngrok_domain_len = 16,
    };
    hu_tunnel_t t = hu_tunnel_create(&alloc, &config);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "ngrok");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_cloudflare_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_cloudflare_tunnel_create(&alloc, "test-token", 10);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_NOT_NULL(t.vtable);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "cloudflare");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_cloudflare_not_running_before_start(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_cloudflare_tunnel_create(&alloc, "x", 1);
    HU_ASSERT_FALSE(t.vtable->is_running(t.ctx));
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_ngrok_not_running_before_start(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_ngrok_tunnel_create(&alloc, "x", 1, NULL, 0);
    HU_ASSERT_FALSE(t.vtable->is_running(t.ctx));
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_custom_different_ports(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *cmd = "echo https://example.com:1234";
    hu_tunnel_t t = hu_custom_tunnel_create(&alloc, cmd, strlen(cmd));
    char *url = NULL;
    size_t url_len = 0;
    hu_tunnel_error_t err = t.vtable->start(t.ctx, 9999, &url, &url_len);
    HU_ASSERT_EQ(err, HU_TUNNEL_ERR_OK);
    HU_ASSERT_NOT_NULL(url);
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_none_multiple_start_stop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_none_tunnel_create(&alloc);
    char *url1 = NULL, *url2 = NULL;
    size_t len1 = 0, len2 = 0;
    t.vtable->start(t.ctx, 3000, &url1, &len1);
    t.vtable->stop(t.ctx);
    t.vtable->start(t.ctx, 4000, &url2, &len2);
    HU_ASSERT_NOT_NULL(url2);
    HU_ASSERT_TRUE(strstr(url2, "4000") != NULL);
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_provider_enum_values(void) {
    HU_ASSERT_EQ(HU_TUNNEL_NONE, 0);
    HU_ASSERT_NEQ(HU_TUNNEL_TAILSCALE, HU_TUNNEL_NONE);
    HU_ASSERT_NEQ(HU_TUNNEL_CLOUDFLARE, HU_TUNNEL_NONE);
}

static void test_tunnel_none_public_url_after_stop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_none_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    t.vtable->start(t.ctx, 8080, &url, &url_len);
    t.vtable->stop(t.ctx);
    const char *pub = t.vtable->public_url(t.ctx);
    HU_ASSERT_NOT_NULL(pub);
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_tailscale_stop(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_tailscale_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    t.vtable->start(t.ctx, 8080, &url, &url_len);
    t.vtable->stop(t.ctx);
    HU_ASSERT_FALSE(t.vtable->is_running(t.ctx));
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_config_provider_cloudflare(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_config_t cfg = {
        .provider = HU_TUNNEL_CLOUDFLARE,
        .cloudflare_token = "t",
        .cloudflare_token_len = 1,
    };
    hu_tunnel_t t = hu_tunnel_create(&alloc, &cfg);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "cloudflare");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

void run_tunnel_tests(void) {
    HU_TEST_SUITE("tunnel");
    HU_RUN_TEST(test_none_tunnel_create);
    HU_RUN_TEST(test_none_tunnel_start_returns_localhost);
    HU_RUN_TEST(test_tunnel_config_parsing);
    HU_RUN_TEST(test_tunnel_create_null_config_uses_none);
    HU_RUN_TEST(test_tunnel_error_string);
    HU_RUN_TEST(test_tailscale_tunnel_create);
    HU_RUN_TEST(test_tailscale_tunnel_start_test_mode);
    HU_RUN_TEST(test_custom_tunnel_create);
    HU_RUN_TEST(test_custom_tunnel_start_test_mode);
    HU_RUN_TEST(test_tunnel_factory_tailscale);
    HU_RUN_TEST(test_tunnel_factory_custom);
    HU_RUN_TEST(test_tunnel_factory_cloudflare);
    HU_RUN_TEST(test_tunnel_factory_ngrok);
    HU_RUN_TEST(test_tunnel_none_is_running_after_start);
    HU_RUN_TEST(test_tunnel_none_public_url);
    HU_RUN_TEST(test_tunnel_error_strings_all);
    HU_RUN_TEST(test_tunnel_create_null_alloc_returns_invalid);
    HU_RUN_TEST(test_tunnel_none_stop);
    HU_RUN_TEST(test_tunnel_custom_url_contains_port_placeholder);
    HU_RUN_TEST(test_tunnel_tailscale_returns_url);
    HU_RUN_TEST(test_tunnel_config_ngrok_domain);
    HU_RUN_TEST(test_tunnel_cloudflare_create);
    HU_RUN_TEST(test_tunnel_cloudflare_not_running_before_start);
    HU_RUN_TEST(test_tunnel_ngrok_not_running_before_start);
    HU_RUN_TEST(test_tunnel_custom_different_ports);
    HU_RUN_TEST(test_tunnel_none_multiple_start_stop);
    HU_RUN_TEST(test_tunnel_provider_enum_values);
    HU_RUN_TEST(test_tunnel_none_public_url_after_stop);
    HU_RUN_TEST(test_tunnel_tailscale_stop);
    HU_RUN_TEST(test_tunnel_config_provider_cloudflare);
}
