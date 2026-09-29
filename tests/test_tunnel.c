#include "human/core/allocator.h"
#include "human/tunnel.h"
#include "test_framework.h"
#include <string.h>

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

static void test_tunnel_provider_enum_values(void) {
    HU_ASSERT_EQ(HU_TUNNEL_NONE, 0);
    HU_ASSERT_NEQ(HU_TUNNEL_TAILSCALE, HU_TUNNEL_NONE);
    HU_ASSERT_NEQ(HU_TUNNEL_CLOUDFLARE, HU_TUNNEL_NONE);
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

void run_tunnel_tests(void) {
    HU_TEST_SUITE("tunnel");
    HU_RUN_TEST(test_tailscale_tunnel_create);
    HU_RUN_TEST(test_tailscale_tunnel_start_test_mode);
    HU_RUN_TEST(test_custom_tunnel_create);
    HU_RUN_TEST(test_custom_tunnel_start_test_mode);
    HU_RUN_TEST(test_tunnel_custom_url_contains_port_placeholder);
    HU_RUN_TEST(test_tunnel_tailscale_returns_url);
    HU_RUN_TEST(test_tunnel_cloudflare_create);
    HU_RUN_TEST(test_tunnel_cloudflare_not_running_before_start);
    HU_RUN_TEST(test_tunnel_ngrok_not_running_before_start);
    HU_RUN_TEST(test_tunnel_custom_different_ports);
    HU_RUN_TEST(test_tunnel_provider_enum_values);
    HU_RUN_TEST(test_tunnel_tailscale_stop);
}
