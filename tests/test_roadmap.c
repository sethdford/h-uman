#include "human/agent.h"
#include "human/agent/mailbox.h"
#include "human/agent/profile.h"
#include "human/agent/spawn.h"
#include "human/channels/thread_binding.h"
#include "human/config.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/daemon.h"
#include "human/mcp_registry.h"
#include "human/observability/otel.h"
#include "human/plugin.h"
#include "human/plugin_loader.h"
#include "human/security/policy_engine.h"
#include "human/security/replay.h"
#include "human/tools/agent_query.h"
#include "human/tools/agent_spawn.h"
#include "human/tools/apply_patch.h"
#include "human/tools/canvas.h"
#include "human/tools/database.h"
#include "human/tools/diff.h"
#include "human/tools/factory.h"
#include "human/tools/notebook.h"
#include "human/update.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

static void test_pool_create_destroy(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *p = hu_agent_pool_create(&a, 4);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_EQ(hu_agent_pool_running_count(p), 0);
    hu_agent_pool_destroy(p);
}

static void test_pool_spawn_one_shot(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *p = hu_agent_pool_create(&a, 4);
    hu_spawn_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider = "openai";
    cfg.provider_len = 6;
    cfg.model = "gpt-4o";
    cfg.model_len = 6;
    cfg.mode = HU_SPAWN_ONE_SHOT;
    uint64_t id = 0;
    HU_ASSERT_EQ(hu_agent_pool_spawn(p, &cfg, "hello", 5, "test", &id), HU_OK);
    HU_ASSERT(id > 0);
    HU_ASSERT_EQ(hu_agent_pool_status(p, id), HU_AGENT_COMPLETED);
    HU_ASSERT_NOT_NULL(hu_agent_pool_result(p, id));
    hu_agent_pool_destroy(p);
}

static void test_pool_spawn_persistent(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *p = hu_agent_pool_create(&a, 4);
    hu_spawn_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider = "openai";
    cfg.provider_len = 6;
    cfg.mode = HU_SPAWN_PERSISTENT;
    uint64_t id = 0;
    HU_ASSERT_EQ(hu_agent_pool_spawn(p, &cfg, "hi", 2, "pers", &id), HU_OK);
    HU_ASSERT_EQ(hu_agent_pool_status(p, id), HU_AGENT_IDLE);
    hu_agent_pool_destroy(p);
}

static void test_pool_cancel(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *p = hu_agent_pool_create(&a, 4);
    hu_spawn_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider = "openai";
    cfg.provider_len = 6;
    uint64_t id = 0;
    hu_agent_pool_spawn(p, &cfg, "t", 1, "c", &id);
    HU_ASSERT_EQ(hu_agent_pool_cancel(p, id), HU_OK);
    HU_ASSERT_EQ(hu_agent_pool_status(p, id), HU_AGENT_CANCELLED);
    HU_ASSERT_EQ(hu_agent_pool_cancel(p, 9999), HU_ERR_NOT_FOUND);
    hu_agent_pool_destroy(p);
}

static void test_pool_list(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *p = hu_agent_pool_create(&a, 4);
    hu_spawn_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider = "openai";
    cfg.provider_len = 6;
    uint64_t id1 = 0, id2 = 0;
    hu_agent_pool_spawn(p, &cfg, "a", 1, "l1", &id1);
    hu_agent_pool_spawn(p, &cfg, "b", 1, "l2", &id2);
    hu_agent_pool_info_t *info = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_agent_pool_list(p, &a, &info, &count), HU_OK);
    HU_ASSERT_EQ(count, 2);
    a.free(a.ctx, info, count * sizeof(hu_agent_pool_info_t));
    hu_agent_pool_destroy(p);
}

static void test_pool_query(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *p = hu_agent_pool_create(&a, 4);
    char *resp = NULL;
    size_t rlen = 0;
    HU_ASSERT_EQ(hu_agent_pool_query(p, 1, "q", 1, &resp, &rlen), HU_OK);
    HU_ASSERT_NOT_NULL(resp);
    if (resp)
        a.free(a.ctx, resp, rlen + 1);
    hu_agent_pool_destroy(p);
}

static void test_mailbox_create_destroy(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mailbox_t *m = hu_mailbox_create(&a, 8);
    HU_ASSERT_NOT_NULL(m);
    hu_mailbox_destroy(m);
}

static void test_mailbox_send_recv(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mailbox_t *m = hu_mailbox_create(&a, 8);
    HU_ASSERT_EQ(hu_mailbox_register(m, 1), HU_OK);
    HU_ASSERT_EQ(hu_mailbox_register(m, 2), HU_OK);
    HU_ASSERT_EQ(hu_mailbox_send(m, 1, 2, HU_MSG_TASK, "hello", 5, 42), HU_OK);
    HU_ASSERT_EQ(hu_mailbox_pending_count(m, 2), 1);
    hu_message_t msg;
    HU_ASSERT_EQ(hu_mailbox_recv(m, 2, &msg), HU_OK);
    HU_ASSERT_EQ(msg.type, HU_MSG_TASK);
    HU_ASSERT_EQ(msg.from_agent, 1);
    HU_ASSERT_EQ(msg.correlation_id, 42);
    HU_ASSERT_STR_EQ(msg.payload, "hello");
    hu_message_free(&a, &msg);
    HU_ASSERT_EQ(hu_mailbox_pending_count(m, 2), 0);
    hu_mailbox_destroy(m);
}

static void test_mailbox_recv_empty(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mailbox_t *m = hu_mailbox_create(&a, 4);
    HU_ASSERT_EQ(hu_mailbox_register(m, 1), HU_OK);
    hu_message_t msg;
    HU_ASSERT_EQ(hu_mailbox_recv(m, 1, &msg), HU_ERR_NOT_FOUND);
    hu_mailbox_destroy(m);
}

static void test_mailbox_broadcast(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mailbox_t *m = hu_mailbox_create(&a, 8);
    hu_mailbox_register(m, 1);
    hu_mailbox_register(m, 2);
    hu_mailbox_register(m, 3);
    HU_ASSERT_EQ(hu_mailbox_broadcast(m, 1, HU_MSG_CANCEL, "stop", 4), HU_OK);
    HU_ASSERT_EQ(hu_mailbox_pending_count(m, 1), 0);
    HU_ASSERT_EQ(hu_mailbox_pending_count(m, 2), 1);
    HU_ASSERT_EQ(hu_mailbox_pending_count(m, 3), 1);
    hu_message_t msg;
    hu_mailbox_recv(m, 2, &msg);
    hu_message_free(&a, &msg);
    hu_mailbox_recv(m, 3, &msg);
    hu_message_free(&a, &msg);
    hu_mailbox_destroy(m);
}

static void test_mailbox_unregister(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mailbox_t *m = hu_mailbox_create(&a, 4);
    HU_ASSERT_EQ(hu_mailbox_register(m, 1), HU_OK);
    HU_ASSERT_EQ(hu_mailbox_send(m, 2, 1, HU_MSG_PING, "p", 1, 0), HU_OK);
    HU_ASSERT_EQ(hu_mailbox_unregister(m, 1), HU_OK);
    HU_ASSERT_EQ(hu_mailbox_send(m, 2, 1, HU_MSG_PING, "p", 1, 0), HU_ERR_NOT_FOUND);
    hu_mailbox_destroy(m);
}

static void test_thread_bind_lookup(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_thread_binding_t *tb = hu_thread_binding_create(&a, 32);
    HU_ASSERT_NOT_NULL(tb);
    HU_ASSERT_EQ(hu_thread_binding_bind(tb, "discord", "thread-123", 42, 300), HU_OK);
    uint64_t aid = 0;
    HU_ASSERT_EQ(hu_thread_binding_lookup(tb, "discord", "thread-123", &aid), HU_OK);
    HU_ASSERT_EQ(aid, 42);
    HU_ASSERT_EQ(hu_thread_binding_count(tb), 1);
    hu_thread_binding_destroy(tb);
}

static void test_thread_unbind(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_thread_binding_t *tb = hu_thread_binding_create(&a, 32);
    hu_thread_binding_bind(tb, "slack", "ts-456", 10, 0);
    HU_ASSERT_EQ(hu_thread_binding_unbind(tb, "slack", "ts-456"), HU_OK);
    uint64_t aid = 0;
    HU_ASSERT_EQ(hu_thread_binding_lookup(tb, "slack", "ts-456", &aid), HU_ERR_NOT_FOUND);
    hu_thread_binding_destroy(tb);
}

static void test_thread_expire(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_thread_binding_t *tb = hu_thread_binding_create(&a, 32);
    hu_thread_binding_bind(tb, "telegram", "chain-1", 5, 10);
    HU_ASSERT_EQ(hu_thread_binding_count(tb), 1);
    size_t expired = hu_thread_binding_expire_idle(tb, (int64_t)time(NULL) + 20);
    HU_ASSERT_EQ(expired, 1);
    HU_ASSERT_EQ(hu_thread_binding_count(tb), 0);
    hu_thread_binding_destroy(tb);
}

static void test_thread_list(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_thread_binding_t *tb = hu_thread_binding_create(&a, 32);
    hu_thread_binding_bind(tb, "discord", "a", 1, 0);
    hu_thread_binding_bind(tb, "slack", "b", 2, 0);
    hu_thread_binding_entry_t *entries = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_thread_binding_list(tb, &a, &entries, &count), HU_OK);
    HU_ASSERT_EQ(count, 2);
    a.free(a.ctx, entries, count * sizeof(hu_thread_binding_entry_t));
    hu_thread_binding_destroy(tb);
}

static void test_agent_spawn_tool(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *pool = hu_agent_pool_create(&a, 4);
    hu_tool_t tool = {0};
    HU_ASSERT_EQ(hu_agent_spawn_tool_create(&a, pool, &tool), HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "agent_spawn");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &a);
    hu_agent_pool_destroy(pool);
}

static void test_agent_query_tool(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *pool = hu_agent_pool_create(&a, 4);
    hu_tool_t tool = {0};
    HU_ASSERT_EQ(hu_agent_query_tool_create(&a, pool, &tool), HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "agent_query");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &a);
    hu_agent_pool_destroy(pool);
}

static void test_canvas_tool(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_tool_t tool = {0};
    HU_ASSERT_EQ(hu_canvas_create(&a, &tool), HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "canvas");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &a);
}

static void test_apply_patch_tool(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_tool_t tool = {0};
    HU_ASSERT_EQ(hu_apply_patch_create(&a, NULL, &tool), HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "apply_patch");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &a);
}

static void test_notebook_tool(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_tool_t tool = {0};
    HU_ASSERT_EQ(hu_notebook_create(&a, NULL, &tool), HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "notebook");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &a);
}

static void test_database_tool(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_tool_t tool = {0};
    HU_ASSERT_EQ(hu_database_tool_create(&a, NULL, &tool), HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "database");
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &a);
}

static void test_diff_tool(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_tool_t tool = {0};
    HU_ASSERT_EQ(hu_diff_tool_create(&a, NULL, 0, NULL, &tool), HU_OK);
    HU_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "diff");
}

static void test_policy_engine_deny(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_policy_engine_t *e = hu_policy_engine_create(&a);
    HU_ASSERT_NOT_NULL(e);
    hu_policy_match_t m = {.tool = "git",
                           .args_contains = "push --force",
                           .session_cost_gt = 0,
                           .has_cost_check = false};
    HU_ASSERT_EQ(hu_policy_engine_add_rule(e, "no-force-push", m, HU_POLICY_DENY, "blocked"),
                 HU_OK);
    hu_policy_eval_ctx_t ctx = {
        .tool_name = "git", .args_json = "{\"cmd\":\"push --force\"}", .session_cost_usd = 0};
    HU_ASSERT_EQ(hu_policy_engine_evaluate(e, &ctx).action, HU_POLICY_DENY);
    hu_policy_eval_ctx_t ctx2 = {
        .tool_name = "git", .args_json = "{\"cmd\":\"push\"}", .session_cost_usd = 0};
    HU_ASSERT_EQ(hu_policy_engine_evaluate(e, &ctx2).action, HU_POLICY_ALLOW);
    hu_policy_engine_destroy(e);
}

static void test_policy_engine_cost(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_policy_engine_t *e = hu_policy_engine_create(&a);
    hu_policy_match_t m = {
        .tool = NULL, .args_contains = NULL, .session_cost_gt = 5.0, .has_cost_check = true};
    hu_policy_engine_add_rule(e, "budget", m, HU_POLICY_DENY, "over budget");
    hu_policy_eval_ctx_t c1 = {.tool_name = "shell", .args_json = NULL, .session_cost_usd = 3.0};
    HU_ASSERT_EQ(hu_policy_engine_evaluate(e, &c1).action, HU_POLICY_ALLOW);
    hu_policy_eval_ctx_t c2 = {.tool_name = "shell", .args_json = NULL, .session_cost_usd = 6.0};
    HU_ASSERT_EQ(hu_policy_engine_evaluate(e, &c2).action, HU_POLICY_DENY);
    hu_policy_engine_destroy(e);
}

static void test_policy_engine_approval(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_policy_engine_t *e = hu_policy_engine_create(&a);
    hu_policy_match_t m = {
        .tool = "shell", .args_contains = "rm -rf", .session_cost_gt = 0, .has_cost_check = false};
    hu_policy_engine_add_rule(e, "approve-rm", m, HU_POLICY_REQUIRE_APPROVAL, "needs approval");
    hu_policy_eval_ctx_t c = {
        .tool_name = "shell", .args_json = "rm -rf /tmp", .session_cost_usd = 0};
    HU_ASSERT_EQ(hu_policy_engine_evaluate(e, &c).action, HU_POLICY_REQUIRE_APPROVAL);
    hu_policy_engine_destroy(e);
}

static void test_otel_observer(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_observer_t obs = {0};
    hu_otel_config_t cfg = {.endpoint = "http://localhost:4317",
                            .endpoint_len = 21,
                            .service_name = "human",
                            .service_name_len = 7,
                            .enable_traces = true,
                            .enable_metrics = true,
                            .enable_logs = true};
    HU_ASSERT_EQ(hu_otel_observer_create(&a, &cfg, &obs), HU_OK);
    HU_ASSERT_STR_EQ(obs.vtable->name(obs.ctx), "otel");
    if (obs.vtable->deinit)
        obs.vtable->deinit(obs.ctx);
}

static void test_otel_span(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_span_t *span = hu_span_start(&a, "test_span", 9);
    HU_ASSERT_NOT_NULL(span);
    hu_span_set_attr_str(span, "tool", "shell");
    hu_span_set_attr_int(span, "tokens", 100);
    hu_span_set_attr_double(span, "cost", 0.05);
    HU_ASSERT_EQ(hu_span_end(span, &a), HU_OK);
}

static void test_replay_record(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_replay_recorder_t *r = hu_replay_recorder_create(&a, 100);
    HU_ASSERT_NOT_NULL(r);
    HU_ASSERT_EQ(hu_replay_record(r, HU_REPLAY_TOOL_CALL, "{\"tool\":\"shell\"}", 16), HU_OK);
    HU_ASSERT_EQ(hu_replay_record(r, HU_REPLAY_TOOL_RESULT, "ok", 2), HU_OK);
    HU_ASSERT_EQ(hu_replay_event_count(r), 2);
    hu_replay_event_t ev;
    HU_ASSERT_EQ(hu_replay_get_event(r, 0, &ev), HU_OK);
    HU_ASSERT_EQ(ev.type, HU_REPLAY_TOOL_CALL);
    char *json = NULL;
    size_t jlen = 0;
    HU_ASSERT_EQ(hu_replay_export_json(r, &a, &json, &jlen), HU_OK);
    HU_ASSERT_NOT_NULL(json);
    a.free(a.ctx, json, jlen + 1);
    hu_replay_recorder_destroy(r);
}

static void test_plugin_registry(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 16);
    HU_ASSERT_NOT_NULL(reg);
    hu_plugin_info_t info = {.name = "test-plugin",
                             .version = "1.0.0",
                             .description = "A test plugin",
                             .api_version = HU_PLUGIN_API_VERSION};
    HU_ASSERT_EQ(hu_plugin_register(reg, &info, NULL, 0), HU_OK);
    HU_ASSERT_EQ(hu_plugin_count(reg), 1);
    hu_plugin_info_t out;
    HU_ASSERT_EQ(hu_plugin_get_info(reg, 0, &out), HU_OK);
    HU_ASSERT_STR_EQ(out.name, "test-plugin");
    hu_plugin_registry_destroy(reg);
}

static void test_plugin_bad_version(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 4);
    hu_plugin_info_t info = {
        .name = "bad", .version = "0.0.1", .description = NULL, .api_version = 999};
    HU_ASSERT_EQ(hu_plugin_register(reg, &info, NULL, 0), HU_ERR_INVALID_ARGUMENT);
    hu_plugin_registry_destroy(reg);
}

static void test_plugin_load_nonexistent(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_host_t host = {
        .alloc = &a, .register_tool = NULL, .register_provider = NULL, .host_ctx = NULL};
    hu_plugin_info_t info = {0};
    hu_plugin_handle_t *handle = NULL;
    hu_error_t err = hu_plugin_load(&a, "/nonexistent/plugin.so", &host, &info, &handle);
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    HU_ASSERT_NULL(handle);
}

static void test_plugin_load_api_mismatch(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_host_t host = {.alloc = &a};
    hu_plugin_info_t info = {0};
    hu_plugin_handle_t *handle = NULL;
    hu_error_t err = hu_plugin_load(&a, "/bad_api/plugin.so", &host, &info, &handle);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(handle);
}

static void test_mcp_registry_add_remove_list(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mcp_registry_t *reg = hu_mcp_registry_create(&a);
    HU_ASSERT_NOT_NULL(reg);
    HU_ASSERT_EQ(hu_mcp_registry_add(reg, "srv1", "echo", "hello"), HU_OK);
    HU_ASSERT_EQ(hu_mcp_registry_add(reg, "srv2", "cat", NULL), HU_OK);
    hu_mcp_registry_entry_t out[4];
    int count = 0;
    HU_ASSERT_EQ(hu_mcp_registry_list(reg, out, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 2);
    HU_ASSERT_STR_EQ(out[0].name, "srv1");
    HU_ASSERT_EQ(hu_mcp_registry_remove(reg, "srv1"), HU_OK);
    HU_ASSERT_EQ(hu_mcp_registry_list(reg, out, 4, &count), HU_OK);
    HU_ASSERT_EQ(count, 1);
    hu_mcp_registry_destroy(reg);
}

static void test_mcp_registry_start_stop_toggles_running(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mcp_registry_t *reg = hu_mcp_registry_create(&a);
    HU_ASSERT_NOT_NULL(reg);
    HU_ASSERT_EQ(hu_mcp_registry_add(reg, "srv", "echo", "x"), HU_OK);
    HU_ASSERT_EQ(hu_mcp_registry_start(reg, "srv"), HU_OK);
    hu_mcp_registry_entry_t out[1];
    int count = 0;
    HU_ASSERT_EQ(hu_mcp_registry_list(reg, out, 1, &count), HU_OK);
    HU_ASSERT_TRUE(out[0].running);
    HU_ASSERT_EQ(hu_mcp_registry_stop(reg, "srv"), HU_OK);
    HU_ASSERT_EQ(hu_mcp_registry_list(reg, out, 1, &count), HU_OK);
    HU_ASSERT_FALSE(out[0].running);
    hu_mcp_registry_destroy(reg);
}

static void test_mcp_registry_remove_nonexistent(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mcp_registry_t *reg = hu_mcp_registry_create(&a);
    HU_ASSERT_NOT_NULL(reg);
    hu_error_t err = hu_mcp_registry_remove(reg, "nonexistent");
    HU_ASSERT_EQ(err, HU_ERR_NOT_FOUND);
    hu_mcp_registry_destroy(reg);
}

static void test_profile_get_coding(void) {
    const hu_agent_profile_t *p = hu_agent_profile_get(HU_PROFILE_CODING);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_EQ(p->name, "coding");
    HU_ASSERT(p->default_tools_count > 0);
    HU_ASSERT_EQ(p->autonomy_level, 2);
}

static void test_profile_get_ops(void) {
    const hu_agent_profile_t *p = hu_agent_profile_get(HU_PROFILE_OPS);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_EQ(p->name, "ops");
}

static void test_profile_get_messaging(void) {
    const hu_agent_profile_t *p = hu_agent_profile_get(HU_PROFILE_MESSAGING);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_EQ(p->name, "messaging");
}

static void test_profile_get_minimal(void) {
    const hu_agent_profile_t *p = hu_agent_profile_get(HU_PROFILE_MINIMAL);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_STR_EQ(p->name, "minimal");
    HU_ASSERT_EQ(p->autonomy_level, 0);
}

static void test_profile_by_name(void) {
    const hu_agent_profile_t *p = hu_agent_profile_by_name("coding", 6);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_EQ(p->type, HU_PROFILE_CODING);
    HU_ASSERT_NULL(hu_agent_profile_by_name("nonexist", 8));
}

static void test_profile_list(void) {
    const hu_agent_profile_t *profiles = NULL;
    size_t count = 0;
    HU_ASSERT_EQ(hu_agent_profile_list(&profiles, &count), HU_OK);
    HU_ASSERT(count >= 4);
    HU_ASSERT_NOT_NULL(profiles);
}

static void test_pool_null_alloc(void) {
    HU_ASSERT_NULL(hu_agent_pool_create(NULL, 4));
}

static void test_pool_zero_max(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *pool = hu_agent_pool_create(&a, 0);
    HU_ASSERT_NOT_NULL(pool);
    HU_ASSERT_EQ(hu_agent_pool_running_count(pool), 0);
    hu_agent_pool_destroy(pool);
}

static void test_pool_status_invalid_id(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *pool = hu_agent_pool_create(&a, 2);
    HU_ASSERT_EQ(hu_agent_pool_status(pool, 99999), HU_AGENT_FAILED);
    hu_agent_pool_destroy(pool);
}

static void test_pool_cancel_invalid_id(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *pool = hu_agent_pool_create(&a, 2);
    HU_ASSERT_EQ(hu_agent_pool_cancel(pool, 99999), HU_ERR_NOT_FOUND);
    hu_agent_pool_destroy(pool);
}

static void test_pool_result_invalid_id(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *pool = hu_agent_pool_create(&a, 2);
    HU_ASSERT_NULL(hu_agent_pool_result(pool, 99999));
    hu_agent_pool_destroy(pool);
}

static void test_canvas_description(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_tool_t tool = {0};
    HU_ASSERT_EQ(hu_canvas_create(&a, &tool), HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &a);
}

static void test_apply_patch_description(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_tool_t tool = {0};
    HU_ASSERT_EQ(hu_apply_patch_create(&a, NULL, &tool), HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &a);
}

static void test_notebook_description(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_tool_t tool = {0};
    HU_ASSERT_EQ(hu_notebook_create(&a, NULL, &tool), HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &a);
}

static void test_diff_description(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_tool_t tool = {0};
    HU_ASSERT_EQ(hu_diff_tool_create(&a, NULL, 0, NULL, &tool), HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
}

static void test_database_description(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_tool_t tool = {0};
    HU_ASSERT_EQ(hu_database_tool_create(&a, NULL, &tool), HU_OK);
    HU_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    HU_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &a);
}

static void test_policy_engine_null(void) {
    HU_ASSERT_NULL(hu_policy_engine_create(NULL));
}

static void test_policy_engine_no_rules_allows(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_policy_engine_t *e = hu_policy_engine_create(&a);
    hu_policy_eval_ctx_t ctx = {.tool_name = "shell", .args_json = NULL, .session_cost_usd = 0};
    HU_ASSERT_EQ(hu_policy_engine_evaluate(e, &ctx).action, HU_POLICY_ALLOW);
    hu_policy_engine_destroy(e);
}

static void test_mailbox_send_unregistered_target(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mailbox_t *mb = hu_mailbox_create(&a, 4);
    HU_ASSERT_EQ(hu_mailbox_send(mb, 0, 99, HU_MSG_TASK, "hi", 2, 0), HU_ERR_NOT_FOUND);
    hu_mailbox_destroy(mb);
}

static void test_profile_by_name_null(void) {
    HU_ASSERT_NULL(hu_agent_profile_by_name(NULL, 0));
    HU_ASSERT_NULL(hu_agent_profile_by_name("", 0));
}

static void test_daemon_start_returns_valid(void) {
    hu_error_t err = hu_daemon_start();
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_SUPPORTED || err == HU_ERR_INVALID_ARGUMENT);
}

static void test_update_check_returns_valid(void) {
    char vbuf[64];
    hu_error_t err = hu_update_check(vbuf, sizeof(vbuf));
    HU_ASSERT_TRUE(err == HU_OK || err == HU_ERR_NOT_SUPPORTED);
}

static void test_update_check_null_buf(void) {
    hu_error_t err = hu_update_check(NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_integ_config_defaults(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_error_t err = hu_config_load(&a, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cfg.agent.pool_max_concurrent, 8);
    HU_ASSERT_NULL(cfg.agent.default_profile);
    HU_ASSERT_EQ(cfg.policy.enabled, false);
    HU_ASSERT_EQ(cfg.plugins.enabled, false);
    hu_config_deinit(&cfg);
}

static void test_integ_config_parse_pool(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_error_t err = hu_config_load(&a, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    const char *json = "{\"agent\":{\"pool_max_concurrent\":16}}";
    err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cfg.agent.pool_max_concurrent, 16);
    hu_config_deinit(&cfg);
}

static void test_integ_config_parse_policy(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    hu_error_t err = hu_config_load(&a, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    const char *json = "{\"policy\":{\"enabled\":true}}";
    err = hu_config_parse_json(&cfg, json, strlen(json));
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(cfg.policy.enabled, true);
    hu_config_deinit(&cfg);
}

static void test_integ_agent_fields(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    HU_ASSERT_NULL(agent.agent_pool);
    HU_ASSERT_NULL(agent.mailbox);
    HU_ASSERT_NULL(agent.policy_engine);
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *pool = hu_agent_pool_create(&a, 4);
    hu_mailbox_t *mb = hu_mailbox_create(&a, 8);
    hu_policy_engine_t *pe = hu_policy_engine_create(&a);
    agent.agent_pool = pool;
    agent.mailbox = mb;
    agent.policy_engine = pe;
    HU_ASSERT_NOT_NULL(agent.agent_pool);
    HU_ASSERT_NOT_NULL(agent.mailbox);
    HU_ASSERT_NOT_NULL(agent.policy_engine);
    hu_policy_engine_destroy(pe);
    hu_mailbox_destroy(mb);
    hu_agent_pool_destroy(pool);
}

static void test_integ_factory_pool(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *pool = hu_agent_pool_create(&a, 2);
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&a, ".", 1, NULL, NULL, NULL, NULL, pool, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT(count > 0);
    hu_tools_destroy_default(&a, tools, count);
    hu_agent_pool_destroy(pool);
}

static void test_integ_factory_null_pool(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_tool_t *tools = NULL;
    size_t count = 0;
    hu_error_t err =
        hu_tools_create_default(&a, ".", 1, NULL, NULL, NULL, NULL, NULL, NULL, &tools, &count);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tools_destroy_default(&a, tools, count);
}

static void test_integ_policy_deny(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_policy_engine_t *pe = hu_policy_engine_create(&a);
    hu_policy_match_t m = {.tool = "shell"};
    hu_policy_engine_add_rule(pe, "d", m, HU_POLICY_DENY, "no");
    hu_policy_eval_ctx_t ctx = {.tool_name = "shell", .args_json = "{}"};
    hu_policy_result_t res = hu_policy_engine_evaluate(pe, &ctx);
    HU_ASSERT_EQ(res.action, HU_POLICY_DENY);
    hu_policy_engine_destroy(pe);
}

static void test_integ_policy_allow(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_policy_engine_t *pe = hu_policy_engine_create(&a);
    hu_policy_match_t m = {.tool = "shell"};
    hu_policy_engine_add_rule(pe, "d", m, HU_POLICY_DENY, "no");
    hu_policy_eval_ctx_t ctx = {.tool_name = "file_read", .args_json = "{}"};
    hu_policy_result_t res = hu_policy_engine_evaluate(pe, &ctx);
    HU_ASSERT_EQ(res.action, HU_POLICY_ALLOW);
    hu_policy_engine_destroy(pe);
}

static void test_integ_profile_coding(void) {
    const hu_agent_profile_t *p = hu_agent_profile_get(HU_PROFILE_CODING);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT(p->max_iterations > 0);
    HU_ASSERT(p->max_history > 0);
    HU_ASSERT_NOT_NULL(p->preferred_model);
}

static void test_integ_pool_roundtrip(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_agent_pool_t *pool = hu_agent_pool_create(&a, 4);
    hu_spawn_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.mode = HU_SPAWN_ONE_SHOT;
    cfg.max_iterations = 5;
    uint64_t id = 0;
    HU_ASSERT_EQ(hu_agent_pool_spawn(pool, &cfg, "t", 1, "l", &id), HU_OK);
    HU_ASSERT(id > 0);
    hu_agent_pool_info_t *info = NULL;
    size_t ic = 0;
    HU_ASSERT_EQ(hu_agent_pool_list(pool, &a, &info, &ic), HU_OK);
    HU_ASSERT(ic >= 1);
    if (info)
        a.free(a.ctx, info, ic * sizeof(hu_agent_pool_info_t));
    HU_ASSERT_EQ(hu_agent_pool_cancel(pool, id), HU_OK);
    hu_agent_pool_destroy(pool);
}

static void test_integ_mailbox_roundtrip(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_mailbox_t *mb = hu_mailbox_create(&a, 8);
    HU_ASSERT_EQ(hu_mailbox_register(mb, 10), HU_OK);
    HU_ASSERT_EQ(hu_mailbox_register(mb, 20), HU_OK);
    HU_ASSERT_EQ(hu_mailbox_send(mb, 10, 20, HU_MSG_TASK, "hi", 2, 42), HU_OK);
    hu_message_t msg;
    HU_ASSERT_EQ(hu_mailbox_recv(mb, 20, &msg), HU_OK);
    HU_ASSERT_EQ(msg.type, HU_MSG_TASK);
    HU_ASSERT_STR_EQ(msg.payload, "hi");
    a.free(a.ctx, msg.payload, msg.payload_len + 1);
    hu_mailbox_destroy(mb);
}

static void test_integ_thread_binding(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_thread_binding_t *tb = hu_thread_binding_create(&a, 16);
    HU_ASSERT_NOT_NULL(tb);
    HU_ASSERT_EQ(hu_thread_binding_bind(tb, "discord", "thread-123", 42, 3600), HU_OK);
    uint64_t aid = 0;
    HU_ASSERT_EQ(hu_thread_binding_lookup(tb, "discord", "thread-123", &aid), HU_OK);
    HU_ASSERT_EQ(aid, 42);
    HU_ASSERT_EQ(hu_thread_binding_count(tb), 1);
    hu_thread_binding_destroy(tb);
}

static void test_integ_plugin_registry(void) {
    hu_allocator_t a = hu_system_allocator();
    hu_plugin_registry_t *reg = hu_plugin_registry_create(&a, 8);
    HU_ASSERT_NOT_NULL(reg);
    HU_ASSERT_EQ(hu_plugin_count(reg), 0);
    hu_plugin_registry_destroy(reg);
}

static void test_update_apply_test_mode(void) {
    hu_error_t err = hu_update_apply();
    HU_ASSERT(err == HU_OK);
}

void run_roadmap_tests(void) {
    HU_TEST_SUITE("Roadmap: Agent Pool (1A)");
    HU_RUN_TEST(test_pool_create_destroy);
    HU_RUN_TEST(test_pool_spawn_one_shot);
    HU_RUN_TEST(test_pool_spawn_persistent);
    HU_RUN_TEST(test_pool_cancel);
    HU_RUN_TEST(test_pool_list);
    HU_RUN_TEST(test_pool_query);
    HU_RUN_TEST(test_pool_null_alloc);
    HU_RUN_TEST(test_pool_zero_max);
    HU_RUN_TEST(test_pool_status_invalid_id);
    HU_RUN_TEST(test_pool_cancel_invalid_id);
    HU_RUN_TEST(test_pool_result_invalid_id);

    HU_TEST_SUITE("Roadmap: Mailbox (1B)");
    HU_RUN_TEST(test_mailbox_create_destroy);
    HU_RUN_TEST(test_mailbox_send_recv);
    HU_RUN_TEST(test_mailbox_recv_empty);
    HU_RUN_TEST(test_mailbox_broadcast);
    HU_RUN_TEST(test_mailbox_unregister);
    HU_RUN_TEST(test_mailbox_send_unregistered_target);

    HU_TEST_SUITE("Roadmap: Thread Binding (1C)");
    HU_RUN_TEST(test_thread_bind_lookup);
    HU_RUN_TEST(test_thread_unbind);
    HU_RUN_TEST(test_thread_expire);
    HU_RUN_TEST(test_thread_list);

    HU_TEST_SUITE("Roadmap: Agent Tools (1D+E)");
    HU_RUN_TEST(test_agent_spawn_tool);
    HU_RUN_TEST(test_agent_query_tool);

    HU_TEST_SUITE("Roadmap: Agent Profiles (2)");
    HU_RUN_TEST(test_profile_get_coding);
    HU_RUN_TEST(test_profile_get_ops);
    HU_RUN_TEST(test_profile_get_messaging);
    HU_RUN_TEST(test_profile_get_minimal);
    HU_RUN_TEST(test_profile_by_name);
    HU_RUN_TEST(test_profile_by_name_null);
    HU_RUN_TEST(test_profile_list);

    HU_TEST_SUITE("Roadmap: Power Tools (3)");
    HU_RUN_TEST(test_canvas_tool);
    HU_RUN_TEST(test_canvas_description);
    HU_RUN_TEST(test_apply_patch_tool);
    HU_RUN_TEST(test_apply_patch_description);
    HU_RUN_TEST(test_notebook_tool);
    HU_RUN_TEST(test_notebook_description);
    HU_RUN_TEST(test_database_tool);
    HU_RUN_TEST(test_database_description);
    HU_RUN_TEST(test_diff_tool);
    HU_RUN_TEST(test_diff_description);

    HU_TEST_SUITE("Roadmap: Policy Engine (4A)");
    HU_RUN_TEST(test_policy_engine_deny);
    HU_RUN_TEST(test_policy_engine_cost);
    HU_RUN_TEST(test_policy_engine_approval);
    HU_RUN_TEST(test_policy_engine_null);
    HU_RUN_TEST(test_policy_engine_no_rules_allows);

    HU_TEST_SUITE("Roadmap: OTel + Tracing (4B)");
    HU_RUN_TEST(test_otel_observer);
    HU_RUN_TEST(test_otel_span);

    HU_TEST_SUITE("Roadmap: Action Replay (4D)");
    HU_RUN_TEST(test_replay_record);

    HU_TEST_SUITE("Roadmap: Plugin System (5B)");
    HU_RUN_TEST(test_plugin_registry);
    HU_RUN_TEST(test_plugin_bad_version);
    HU_RUN_TEST(test_plugin_load_nonexistent);
    HU_RUN_TEST(test_plugin_load_api_mismatch);

    HU_TEST_SUITE("Roadmap: MCP Registry");
    HU_RUN_TEST(test_mcp_registry_add_remove_list);
    HU_RUN_TEST(test_mcp_registry_start_stop_toggles_running);
    HU_RUN_TEST(test_mcp_registry_remove_nonexistent);

    HU_TEST_SUITE("Roadmap: Stub Boundaries (6)");
    HU_RUN_TEST(test_daemon_start_returns_valid);
    HU_RUN_TEST(test_update_check_returns_valid);
    HU_RUN_TEST(test_update_check_null_buf);
    HU_RUN_TEST(test_update_apply_test_mode);
    HU_TEST_SUITE("Integration");
    HU_RUN_TEST(test_integ_config_defaults);
    HU_RUN_TEST(test_integ_config_parse_pool);
    HU_RUN_TEST(test_integ_config_parse_policy);
    HU_RUN_TEST(test_integ_agent_fields);
    HU_RUN_TEST(test_integ_factory_pool);
    HU_RUN_TEST(test_integ_factory_null_pool);
    HU_RUN_TEST(test_integ_policy_deny);
    HU_RUN_TEST(test_integ_policy_allow);
    HU_RUN_TEST(test_integ_profile_coding);
    HU_RUN_TEST(test_integ_pool_roundtrip);
    HU_RUN_TEST(test_integ_mailbox_roundtrip);
    HU_RUN_TEST(test_integ_thread_binding);
    HU_RUN_TEST(test_integ_plugin_registry);

    HU_RUN_TEST(test_update_check_returns_valid);
    HU_RUN_TEST(test_update_check_null_buf);
    HU_RUN_TEST(test_update_apply_test_mode);
}
