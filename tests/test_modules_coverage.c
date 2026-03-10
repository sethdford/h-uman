#include "human/agent/mailbox.h"
#include "human/channel.h"
#include "human/channels/dispatch.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/daemon.h"
#include "human/mcp.h"
#include "human/memory.h"
#include "human/memory/retrieval.h"
#include "human/security.h"
#include "human/tool.h"
#include "human/tools/apply_patch.h"
#include "human/tools/diff.h"
#include "human/tools/send_message.h"
#include "human/tunnel.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

/* ─── MCP tests ─────────────────────────────────────────────────────────── */

static void test_mcp_server_create_destroy_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_server_config_t cfg = {.command = "echo", .args = NULL, .args_count = 0};
    hu_mcp_server_t *srv = hu_mcp_server_create(&alloc, &cfg);
    HU_ASSERT_NOT_NULL(srv);
    hu_mcp_server_destroy(srv);
}

static void test_mcp_server_create_null_config_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mcp_server_t *srv = hu_mcp_server_create(&alloc, NULL);
    HU_ASSERT_NULL(srv);
}

static void test_mcp_init_tools_empty_configs_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err = hu_mcp_init_tools(&alloc, NULL, 0, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(tools);
    HU_ASSERT_EQ(count, 0u);
}

/* ─── Tunnel tests ───────────────────────────────────────────────────────── */

static void test_tunnel_none_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_none_tunnel_create(&alloc);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_NOT_NULL(t.vtable);
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_tailscale_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_tailscale_tunnel_create(&alloc);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "tailscale");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_cloudflare_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_cloudflare_tunnel_create(&alloc, "test-token", 10);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "cloudflare");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_ngrok_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_t t = hu_ngrok_tunnel_create(&alloc, "tok", 3, NULL, 0);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "ngrok");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_custom_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *cmd = "echo https://example.com";
    hu_tunnel_t t = hu_custom_tunnel_create(&alloc, cmd, strlen(cmd));
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "custom");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

static void test_tunnel_factory_none(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tunnel_config_t config = {.provider = HU_TUNNEL_NONE};
    hu_tunnel_t t = hu_tunnel_create(&alloc, &config);
    HU_ASSERT_NOT_NULL(t.ctx);
    HU_ASSERT_STR_EQ(t.vtable->provider_name(t.ctx), "none");
    if (t.vtable->deinit)
        t.vtable->deinit(t.ctx, &alloc);
}

/* ─── Daemon tests ───────────────────────────────────────────────────────── */

static void test_daemon_start_stop_test_mode(void) {
#if HU_IS_TEST
    hu_error_t err = hu_daemon_start();
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_daemon_stop();
    HU_ASSERT_EQ(err, HU_OK);
    bool status = hu_daemon_status();
    HU_ASSERT_FALSE(status);
#else
    (void)0;
#endif
}

static void test_cron_schedule_matches_star(void) {
    struct tm tm = {0};
    tm.tm_min = 30;
    tm.tm_hour = 14;
    tm.tm_mday = 15;
    tm.tm_mon = 5;  /* June = month 6 */
    tm.tm_wday = 3; /* Wednesday */
    bool m = hu_cron_schedule_matches("* * * * *", &tm);
    HU_ASSERT_TRUE(m);
}

static void test_cron_schedule_matches_exact(void) {
    struct tm tm = {0};
    tm.tm_min = 30;
    tm.tm_hour = 14;
    tm.tm_mday = 15;
    tm.tm_mon = 5;
    tm.tm_wday = 3;
    bool m = hu_cron_schedule_matches("30 14 15 6 3", &tm);
    HU_ASSERT_TRUE(m);
}

static void test_cron_schedule_matches_no_match(void) {
    struct tm tm = {0};
    tm.tm_min = 0;
    tm.tm_hour = 12;
    tm.tm_mday = 1;
    tm.tm_mon = 0;
    tm.tm_wday = 1;
    bool m = hu_cron_schedule_matches("59 23 31 12 0", &tm);
    HU_ASSERT_FALSE(m);
}

static void test_cron_schedule_matches_null_returns_false(void) {
    struct tm tm = {0};
    HU_ASSERT_FALSE(hu_cron_schedule_matches(NULL, &tm));
    HU_ASSERT_FALSE(hu_cron_schedule_matches("* * * * *", NULL));
}

/* ─── Tools tests (diff, apply_patch, send_message) ──────────────────────── */

static void test_diff_tool_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_diff_tool_create(&alloc, NULL, 0, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "diff");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_diff_tool_create_null_out_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_diff_tool_create(&alloc, NULL, 0, NULL, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_diff_tool_execute_null_args_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_diff_tool_create(&alloc, NULL, 0, NULL, &tool);
    hu_tool_result_t res = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, NULL, &res);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_diff_rejects_path_traversal(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_diff_tool_create(&alloc, "/tmp", 4, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "diff", 4));
    hu_json_object_set(&alloc, args, "file_a", hu_json_string_new(&alloc, "../etc/passwd", 13));
    hu_json_object_set(&alloc, args, "file_b", hu_json_string_new(&alloc, "b.txt", 5));
    hu_tool_result_t result = {0};
    err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(!result.success);
    HU_ASSERT_NOT_NULL(result.error_msg);
    HU_ASSERT_TRUE(strstr(result.error_msg, "path not allowed") != NULL);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_diff_rejects_absolute_path(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t cr = hu_diff_tool_create(&alloc, "/tmp", 4, NULL, &tool);
    HU_ASSERT_EQ(cr, HU_OK);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "action", hu_json_string_new(&alloc, "diff", 4));
    hu_json_object_set(&alloc, args, "file_a", hu_json_string_new(&alloc, "/etc/passwd", 11));
    hu_json_object_set(&alloc, args, "file_b", hu_json_string_new(&alloc, "b.txt", 5));
    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(!result.success);
    HU_ASSERT_NOT_NULL(result.error_msg);
    HU_ASSERT_TRUE(strstr(result.error_msg, "path not allowed") != NULL);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_apply_patch_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_security_policy_t policy = {0};
    policy.workspace_dir = "/tmp";
    policy.workspace_only = true;
    hu_tool_t tool = {0};
    hu_error_t err = hu_apply_patch_create(&alloc, &policy, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "apply_patch");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_apply_patch_create_null_out_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_security_policy_t policy = {0};
    hu_error_t err = hu_apply_patch_create(&alloc, &policy, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_apply_patch_rejects_path_traversal(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_security_policy_t policy = {0};
    policy.workspace_dir = "/tmp";
    policy.workspace_only = true;
    hu_tool_t tool = {0};
    hu_error_t cr = hu_apply_patch_create(&alloc, &policy, &tool);
    HU_ASSERT_EQ(cr, HU_OK);
    hu_json_value_t *args = hu_json_object_new(&alloc);
    hu_json_object_set(&alloc, args, "file", hu_json_string_new(&alloc, "../etc/passwd", 13));
    hu_json_object_set(&alloc, args, "patch",
                       hu_json_string_new(&alloc, "@@ -1 +1 @@\n+line\n", 18));
    hu_tool_result_t result = {0};
    hu_error_t err = tool.vtable->execute(tool.ctx, &alloc, args, &result);
    hu_json_free(&alloc, args);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(!result.success);
    HU_ASSERT_NOT_NULL(result.error_msg);
    HU_ASSERT_TRUE(strstr(result.error_msg, "path traversal") != NULL ||
                   strstr(result.error_msg, "path not allowed") != NULL);
    hu_tool_result_free(&alloc, &result);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_send_message_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mailbox_t *mbox = hu_mailbox_create(&alloc, 4);
    HU_ASSERT_NOT_NULL(mbox);
    hu_tool_t tool = {0};
    hu_error_t err = hu_send_message_create(&alloc, mbox, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "send_message");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
    hu_mailbox_destroy(mbox);
}

static void test_send_message_create_null_mailbox(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool = {0};
    hu_error_t err = hu_send_message_create(&alloc, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Memory retrieval tests ────────────────────────────────────────────── */

static void test_retrieval_keyword_empty_backend(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
        .use_reranking = false,
        .temporal_decay_factor = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = hu_keyword_retrieve(&alloc, &mem, "query", 5, &opts, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.count, 0u);
    mem.vtable->deinit(mem.ctx);
}

static void test_retrieval_hybrid_empty_backend(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_HYBRID,
        .limit = 5,
        .min_score = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, "query", 5, &opts, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.count, 0u);
    mem.vtable->deinit(mem.ctx);
}

static void test_retrieval_engine_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_retrieval_engine_t eng = hu_retrieval_create(&alloc, &mem);
    HU_ASSERT_NOT_NULL(eng.ctx);
    HU_ASSERT_NOT_NULL(eng.vtable);
    eng.vtable->deinit(eng.ctx, &alloc);
    mem.vtable->deinit(mem.ctx);
}

static void test_temporal_decay_score(void) {
    double base = 1.0;
    const char *ts = "2026-01-15T12:00:00Z";
    double d = hu_temporal_decay_score(base, 0.5, ts, strlen(ts));
    HU_ASSERT_TRUE(d >= 0.0);
    HU_ASSERT_TRUE(d <= base);
    double no_decay = hu_temporal_decay_score(base, 0.0, ts, strlen(ts));
    HU_ASSERT_FLOAT_EQ(no_decay, base, 1e-9);
}

#ifdef HU_ENABLE_SQLITE
static void test_retrieval_keyword_with_mock_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "key_a", 5, "alpha beta gamma", 16, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "key_b", 5, "delta epsilon", 14, &cat, NULL, 0);

    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_KEYWORD,
        .limit = 5,
        .min_score = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = hu_keyword_retrieve(&alloc, &mem, "alpha", 5, &opts, &res);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(res.count, 1u);
    HU_ASSERT_STR_EQ(res.entries[0].key, "key_a");
    hu_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
}

static void test_retrieval_hybrid_with_mock_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    hu_memory_category_t cat = {.tag = HU_MEMORY_CATEGORY_CORE};
    mem.vtable->store(mem.ctx, "doc1", 4, "machine learning basics", 23, &cat, NULL, 0);
    mem.vtable->store(mem.ctx, "doc2", 4, "neural network training", 23, &cat, NULL, 0);

    hu_retrieval_options_t opts = {
        .mode = HU_RETRIEVAL_HYBRID,
        .limit = 5,
        .min_score = 0.0,
    };
    hu_retrieval_result_t res = {0};
    hu_error_t err = hu_hybrid_retrieve(&alloc, &mem, NULL, NULL, NULL, "neural", 6, &opts, &res);
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_SUPPORTED || err == HU_ERR_INVALID_ARGUMENT);
    hu_retrieval_result_free(&alloc, &res);
    mem.vtable->deinit(mem.ctx);
}
#endif

/* ─── Channel dispatch tests ─────────────────────────────────────────────── */

static void test_dispatch_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_error_t err = hu_dispatch_create(&alloc, &ch);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(ch.ctx);
    HU_ASSERT_NOT_NULL(ch.vtable);
    HU_ASSERT_STR_EQ(ch.vtable->name(ch.ctx), "dispatch");
    hu_dispatch_destroy(&ch);
    HU_ASSERT_NULL(ch.ctx);
}

static void test_dispatch_create_null_alloc_fails(void) {
    hu_channel_t ch = {0};
    hu_error_t err = hu_dispatch_create(NULL, &ch);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_dispatch_create_null_out_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_dispatch_create(&alloc, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_dispatch_add_channel(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t dispatch_ch = {0};
    hu_dispatch_create(&alloc, &dispatch_ch);

    hu_channel_t sub = {0};
    sub.ctx = (void *)1;
    sub.vtable = NULL;

    hu_error_t err = hu_dispatch_add_channel(&dispatch_ch, &sub);
    HU_ASSERT_EQ(err, HU_OK);

    hu_dispatch_destroy(&dispatch_ch);
}

static void test_dispatch_add_channel_null_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t dispatch_ch = {0};
    hu_dispatch_create(&alloc, &dispatch_ch);
    hu_error_t err = hu_dispatch_add_channel(&dispatch_ch, NULL);
    HU_ASSERT_NEQ(err, HU_OK);
    hu_dispatch_destroy(&dispatch_ch);
}

static void test_dispatch_start_stop_health(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_dispatch_create(&alloc, &ch);
    HU_ASSERT_FALSE(ch.vtable->health_check(ch.ctx));
    hu_error_t err = ch.vtable->start(ch.ctx);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(ch.vtable->health_check(ch.ctx));
    ch.vtable->stop(ch.ctx);
    HU_ASSERT_FALSE(ch.vtable->health_check(ch.ctx));
    hu_dispatch_destroy(&ch);
}

static void test_dispatch_send_test_mode(void) {
#if HU_IS_TEST
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_t ch = {0};
    hu_dispatch_create(&alloc, &ch);
    ch.vtable->start(ch.ctx);
    hu_error_t err = ch.vtable->send(ch.ctx, "target", 6, "hello", 5, NULL, 0);
    HU_ASSERT_EQ(err, HU_OK);
    hu_dispatch_destroy(&ch);
#else
    (void)0;
#endif
}

void run_modules_coverage_tests(void) {
    HU_TEST_SUITE("Modules coverage: MCP");
    HU_RUN_TEST(test_mcp_server_create_destroy_valid);
    HU_RUN_TEST(test_mcp_server_create_null_config_returns_null);
    HU_RUN_TEST(test_mcp_init_tools_empty_configs_returns_ok);

    HU_TEST_SUITE("Modules coverage: Tunnel");
    HU_RUN_TEST(test_tunnel_none_create_destroy);
    HU_RUN_TEST(test_tunnel_tailscale_create_destroy);
    HU_RUN_TEST(test_tunnel_cloudflare_create_destroy);
    HU_RUN_TEST(test_tunnel_ngrok_create_destroy);
    HU_RUN_TEST(test_tunnel_custom_create_destroy);
    HU_RUN_TEST(test_tunnel_factory_none);

    HU_TEST_SUITE("Modules coverage: Daemon");
    HU_RUN_TEST(test_daemon_start_stop_test_mode);
    HU_RUN_TEST(test_cron_schedule_matches_star);
    HU_RUN_TEST(test_cron_schedule_matches_exact);
    HU_RUN_TEST(test_cron_schedule_matches_no_match);
    HU_RUN_TEST(test_cron_schedule_matches_null_returns_false);

    HU_TEST_SUITE("Modules coverage: Tools (diff, apply_patch, send_message)");
    HU_RUN_TEST(test_diff_tool_create);
    HU_RUN_TEST(test_diff_tool_create_null_out_fails);
    HU_RUN_TEST(test_diff_tool_execute_null_args_fails);
    HU_RUN_TEST(test_diff_rejects_path_traversal);
    HU_RUN_TEST(test_diff_rejects_absolute_path);
    HU_RUN_TEST(test_apply_patch_create);
    HU_RUN_TEST(test_apply_patch_create_null_out_fails);
    HU_RUN_TEST(test_apply_patch_rejects_path_traversal);
    HU_RUN_TEST(test_send_message_create);
    HU_RUN_TEST(test_send_message_create_null_mailbox);

    HU_TEST_SUITE("Modules coverage: Memory retrieval");
    HU_RUN_TEST(test_retrieval_keyword_empty_backend);
    HU_RUN_TEST(test_retrieval_hybrid_empty_backend);
    HU_RUN_TEST(test_retrieval_engine_create_destroy);
    HU_RUN_TEST(test_temporal_decay_score);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_retrieval_keyword_with_mock_data);
    HU_RUN_TEST(test_retrieval_hybrid_with_mock_data);
#endif

    HU_TEST_SUITE("Modules coverage: Channel dispatch");
    HU_RUN_TEST(test_dispatch_create_destroy);
    HU_RUN_TEST(test_dispatch_create_null_alloc_fails);
    HU_RUN_TEST(test_dispatch_create_null_out_fails);
    HU_RUN_TEST(test_dispatch_add_channel);
    HU_RUN_TEST(test_dispatch_add_channel_null_fails);
    HU_RUN_TEST(test_dispatch_start_stop_health);
    HU_RUN_TEST(test_dispatch_send_test_mode);
}
