#include "test_framework.h"
#include "seaclaw/tunnel.h"
#include "seaclaw/core/allocator.h"
#include <string.h>

static void test_none_tunnel_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_none_tunnel_create(&alloc);
    SC_ASSERT_NOT_NULL(t.ctx);
    SC_ASSERT_NOT_NULL(t.vtable);
    SC_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "none");
}

static void test_none_tunnel_start_returns_localhost(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_none_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    sc_tunnel_error_t err = t.vtable->start(t.ctx, 8080, &url, &url_len);
    SC_ASSERT_EQ(err, SC_TUNNEL_ERR_OK);
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(strstr(url, "localhost") != NULL);
    SC_ASSERT_TRUE(strstr(url, "8080") != NULL);
}

static void test_tunnel_config_parsing(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_config_t config = {
        .provider = SC_TUNNEL_NONE,
    };
    sc_tunnel_t t = sc_tunnel_create(&alloc, &config);
    SC_ASSERT_NOT_NULL(t.ctx);
    SC_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "none");
}

static void test_tunnel_create_null_config_uses_none(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_tunnel_create(&alloc, NULL);
    SC_ASSERT_NOT_NULL(t.ctx);
    SC_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "none");
}

static void test_tunnel_error_string(void) {
    SC_ASSERT_NOT_NULL(sc_tunnel_error_string(SC_TUNNEL_ERR_OK));
    SC_ASSERT_NOT_NULL(sc_tunnel_error_string(SC_TUNNEL_ERR_START_FAILED));
}

static void test_tailscale_tunnel_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_tailscale_tunnel_create(&alloc);
    SC_ASSERT_NOT_NULL(t.ctx);
    SC_ASSERT_NOT_NULL(t.vtable);
    SC_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "tailscale");
}

static void test_tailscale_tunnel_start_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_tailscale_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    sc_tunnel_error_t err = t.vtable->start(t.ctx, 8080, &url, &url_len);
    SC_ASSERT_EQ(err, SC_TUNNEL_ERR_OK);
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(strstr(url, "ts.net") != NULL);
}

static void test_custom_tunnel_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *cmd = "echo https://example.com";
    sc_tunnel_t t = sc_custom_tunnel_create(&alloc, cmd, strlen(cmd));
    SC_ASSERT_NOT_NULL(t.ctx);
    SC_ASSERT_NOT_NULL(t.vtable);
    SC_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "custom");
}

static void test_custom_tunnel_start_test_mode(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *cmd = "echo https://serveo.example";
    sc_tunnel_t t = sc_custom_tunnel_create(&alloc, cmd, strlen(cmd));
    char *url = NULL;
    size_t url_len = 0;
    sc_tunnel_error_t err = t.vtable->start(t.ctx, 3000, &url, &url_len);
    SC_ASSERT_EQ(err, SC_TUNNEL_ERR_OK);
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(strstr(url, "custom-mock") != NULL);
}

static void test_tunnel_factory_tailscale(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_config_t config = {
        .provider = SC_TUNNEL_TAILSCALE,
    };
    sc_tunnel_t t = sc_tunnel_create(&alloc, &config);
    SC_ASSERT_NOT_NULL(t.ctx);
    SC_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "tailscale");
}

static void test_tunnel_factory_custom(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *cmd = "ssh -R 80:localhost:{port} serveo.net";
    sc_tunnel_config_t config = {
        .provider = SC_TUNNEL_CUSTOM,
        .custom_start_cmd = cmd,
        .custom_start_cmd_len = strlen(cmd),
    };
    sc_tunnel_t t = sc_tunnel_create(&alloc, &config);
    SC_ASSERT_NOT_NULL(t.ctx);
    SC_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "custom");
}

static void test_tunnel_factory_cloudflare(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_config_t config = {
        .provider = SC_TUNNEL_CLOUDFLARE,
        .cloudflare_token = "test-token",
        .cloudflare_token_len = 10,
    };
    sc_tunnel_t t = sc_tunnel_create(&alloc, &config);
    SC_ASSERT_NOT_NULL(t.ctx);
    SC_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "cloudflare");
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_factory_ngrok(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_config_t config = {
        .provider = SC_TUNNEL_NGROK,
        .ngrok_auth_token = "tok",
        .ngrok_auth_token_len = 3,
        .ngrok_domain = NULL,
        .ngrok_domain_len = 0,
    };
    sc_tunnel_t t = sc_tunnel_create(&alloc, &config);
    SC_ASSERT_NOT_NULL(t.ctx);
    SC_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "ngrok");
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_none_is_running_after_start(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_none_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    t.vtable->start(t.ctx, 3000, &url, &url_len);
    SC_ASSERT_TRUE(t.vtable->is_running(t.ctx));
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_none_public_url(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_none_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    t.vtable->start(t.ctx, 8080, &url, &url_len);
    const char *pub = t.vtable->public_url(t.ctx);
    SC_ASSERT_NOT_NULL(pub);
    SC_ASSERT_TRUE(strstr(pub, "localhost") != NULL);
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_error_strings_all(void) {
    SC_ASSERT_NOT_NULL(sc_tunnel_error_string(SC_TUNNEL_ERR_OK));
    SC_ASSERT_NOT_NULL(sc_tunnel_error_string(SC_TUNNEL_ERR_PROCESS_SPAWN));
    SC_ASSERT_NOT_NULL(sc_tunnel_error_string(SC_TUNNEL_ERR_URL_NOT_FOUND));
    SC_ASSERT_NOT_NULL(sc_tunnel_error_string(SC_TUNNEL_ERR_TIMEOUT));
    SC_ASSERT_NOT_NULL(sc_tunnel_error_string(SC_TUNNEL_ERR_INVALID_COMMAND));
    SC_ASSERT_NOT_NULL(sc_tunnel_error_string(SC_TUNNEL_ERR_NOT_IMPLEMENTED));
}

static void test_tunnel_create_null_alloc_returns_invalid(void) {
    sc_tunnel_config_t config = { .provider = SC_TUNNEL_NONE };
    sc_tunnel_t t = sc_tunnel_create(NULL, &config);
    SC_ASSERT_NULL(t.ctx);
    SC_ASSERT_NULL(t.vtable);
}

static void test_tunnel_none_stop(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_none_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    t.vtable->start(t.ctx, 8080, &url, &url_len);
    t.vtable->stop(t.ctx);
    SC_ASSERT_FALSE(t.vtable->is_running(t.ctx));
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_custom_url_contains_port_placeholder(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_custom_tunnel_create(&alloc, "echo https://example.com:{port}", 31);
    char *url = NULL;
    size_t url_len = 0;
    sc_tunnel_error_t err = t.vtable->start(t.ctx, 4567, &url, &url_len);
    SC_ASSERT_EQ(err, SC_TUNNEL_ERR_OK);
    SC_ASSERT_NOT_NULL(url);
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_tailscale_returns_url(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_tailscale_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    sc_tunnel_error_t err = t.vtable->start(t.ctx, 9000, &url, &url_len);
    SC_ASSERT_EQ(err, SC_TUNNEL_ERR_OK);
    SC_ASSERT_NOT_NULL(url);
    SC_ASSERT_TRUE(strstr(url, "ts.net") != NULL);
    SC_ASSERT_NOT_NULL(t.vtable->public_url(t.ctx));
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_config_ngrok_domain(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_config_t config = {
        .provider = SC_TUNNEL_NGROK,
        .ngrok_auth_token = "x",
        .ngrok_auth_token_len = 1,
        .ngrok_domain = "myapp.ngrok.io",
        .ngrok_domain_len = 16,
    };
    sc_tunnel_t t = sc_tunnel_create(&alloc, &config);
    SC_ASSERT_NOT_NULL(t.ctx);
    SC_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "ngrok");
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_cloudflare_create(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_cloudflare_tunnel_create(&alloc, "test-token", 10);
    SC_ASSERT_NOT_NULL(t.ctx);
    SC_ASSERT_NOT_NULL(t.vtable);
    SC_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "cloudflare");
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_cloudflare_not_running_before_start(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_cloudflare_tunnel_create(&alloc, "x", 1);
    SC_ASSERT_FALSE(t.vtable->is_running(t.ctx));
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_ngrok_not_running_before_start(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_ngrok_tunnel_create(&alloc, "x", 1, NULL, 0);
    SC_ASSERT_FALSE(t.vtable->is_running(t.ctx));
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_custom_different_ports(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *cmd = "echo https://example.com:1234";
    sc_tunnel_t t = sc_custom_tunnel_create(&alloc, cmd, strlen(cmd));
    char *url = NULL;
    size_t url_len = 0;
    sc_tunnel_error_t err = t.vtable->start(t.ctx, 9999, &url, &url_len);
    SC_ASSERT_EQ(err, SC_TUNNEL_ERR_OK);
    SC_ASSERT_NOT_NULL(url);
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_none_multiple_start_stop(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_none_tunnel_create(&alloc);
    char *url1 = NULL, *url2 = NULL;
    size_t len1 = 0, len2 = 0;
    t.vtable->start(t.ctx, 3000, &url1, &len1);
    t.vtable->stop(t.ctx);
    t.vtable->start(t.ctx, 4000, &url2, &len2);
    SC_ASSERT_NOT_NULL(url2);
    SC_ASSERT_TRUE(strstr(url2, "4000") != NULL);
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_provider_enum_values(void) {
    SC_ASSERT_EQ(SC_TUNNEL_NONE, 0);
    SC_ASSERT_NEQ(SC_TUNNEL_TAILSCALE, SC_TUNNEL_NONE);
    SC_ASSERT_NEQ(SC_TUNNEL_CLOUDFLARE, SC_TUNNEL_NONE);
}

static void test_tunnel_none_public_url_after_stop(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_none_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    t.vtable->start(t.ctx, 8080, &url, &url_len);
    t.vtable->stop(t.ctx);
    const char *pub = t.vtable->public_url(t.ctx);
    SC_ASSERT_NOT_NULL(pub);
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_tailscale_stop(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_t t = sc_tailscale_tunnel_create(&alloc);
    char *url = NULL;
    size_t url_len = 0;
    t.vtable->start(t.ctx, 8080, &url, &url_len);
    t.vtable->stop(t.ctx);
    SC_ASSERT_FALSE(t.vtable->is_running(t.ctx));
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_config_provider_cloudflare(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tunnel_config_t cfg = {
        .provider = SC_TUNNEL_CLOUDFLARE,
        .cloudflare_token = "t",
        .cloudflare_token_len = 1,
    };
    sc_tunnel_t t = sc_tunnel_create(&alloc, &cfg);
    SC_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "cloudflare");
    if (t.vtable->deinit) t.vtable->deinit(t.ctx, &alloc);
}

void run_tunnel_tests(void) {
    SC_TEST_SUITE("tunnel");
    SC_RUN_TEST(test_none_tunnel_create);
    SC_RUN_TEST(test_none_tunnel_start_returns_localhost);
    SC_RUN_TEST(test_tunnel_config_parsing);
    SC_RUN_TEST(test_tunnel_create_null_config_uses_none);
    SC_RUN_TEST(test_tunnel_error_string);
    SC_RUN_TEST(test_tailscale_tunnel_create);
    SC_RUN_TEST(test_tailscale_tunnel_start_test_mode);
    SC_RUN_TEST(test_custom_tunnel_create);
    SC_RUN_TEST(test_custom_tunnel_start_test_mode);
    SC_RUN_TEST(test_tunnel_factory_tailscale);
    SC_RUN_TEST(test_tunnel_factory_custom);
    SC_RUN_TEST(test_tunnel_factory_cloudflare);
    SC_RUN_TEST(test_tunnel_factory_ngrok);
    SC_RUN_TEST(test_tunnel_none_is_running_after_start);
    SC_RUN_TEST(test_tunnel_none_public_url);
    SC_RUN_TEST(test_tunnel_error_strings_all);
    SC_RUN_TEST(test_tunnel_create_null_alloc_returns_invalid);
    SC_RUN_TEST(test_tunnel_none_stop);
    SC_RUN_TEST(test_tunnel_custom_url_contains_port_placeholder);
    SC_RUN_TEST(test_tunnel_tailscale_returns_url);
    SC_RUN_TEST(test_tunnel_config_ngrok_domain);
    SC_RUN_TEST(test_tunnel_cloudflare_create);
    SC_RUN_TEST(test_tunnel_cloudflare_not_running_before_start);
    SC_RUN_TEST(test_tunnel_ngrok_not_running_before_start);
    SC_RUN_TEST(test_tunnel_custom_different_ports);
    SC_RUN_TEST(test_tunnel_none_multiple_start_stop);
    SC_RUN_TEST(test_tunnel_provider_enum_values);
    SC_RUN_TEST(test_tunnel_none_public_url_after_stop);
    SC_RUN_TEST(test_tunnel_tailscale_stop);
    SC_RUN_TEST(test_tunnel_config_provider_cloudflare);
}
