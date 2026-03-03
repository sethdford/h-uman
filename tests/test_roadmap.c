#include "test_framework.h"
#include "seaclaw/update.h"
#include "seaclaw/daemon.h"
#include "seaclaw/multimodal.h"
#include "seaclaw/voice.h"
#include "seaclaw/websocket.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/string.h"
#include "seaclaw/agent/spawn.h"
#include "seaclaw/agent/mailbox.h"
#include "seaclaw/channels/thread_binding.h"
#include "seaclaw/tools/agent_spawn.h"
#include "seaclaw/tools/agent_query.h"
#include "seaclaw/tools/canvas.h"
#include "seaclaw/tools/apply_patch.h"
#include "seaclaw/tools/notebook.h"
#include "seaclaw/tools/database.h"
#include "seaclaw/tools/diff.h"
#include "seaclaw/security/policy_engine.h"
#include "seaclaw/observability/otel.h"
#include "seaclaw/security/replay.h"
#include "seaclaw/plugin.h"
#include "seaclaw/agent/profile.h"
#include <string.h>
#include <time.h>

static void test_pool_create_destroy(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *p = sc_agent_pool_create(&a, 4);
    SC_ASSERT_NOT_NULL(p);
    SC_ASSERT_EQ(sc_agent_pool_running_count(p), 0);
    sc_agent_pool_destroy(p);
}

static void test_pool_spawn_one_shot(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *p = sc_agent_pool_create(&a, 4);
    sc_spawn_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider = "openai"; cfg.provider_len = 6;
    cfg.model = "gpt-4o"; cfg.model_len = 6;
    cfg.mode = SC_SPAWN_ONE_SHOT;
    uint64_t id = 0;
    SC_ASSERT_EQ(sc_agent_pool_spawn(p, &cfg, "hello", 5, "test", &id), SC_OK);
    SC_ASSERT(id > 0);
    SC_ASSERT_EQ(sc_agent_pool_status(p, id), SC_AGENT_COMPLETED);
    SC_ASSERT_NOT_NULL(sc_agent_pool_result(p, id));
    sc_agent_pool_destroy(p);
}

static void test_pool_spawn_persistent(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *p = sc_agent_pool_create(&a, 4);
    sc_spawn_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider = "openai"; cfg.provider_len = 6;
    cfg.mode = SC_SPAWN_PERSISTENT;
    uint64_t id = 0;
    SC_ASSERT_EQ(sc_agent_pool_spawn(p, &cfg, "hi", 2, "pers", &id), SC_OK);
    SC_ASSERT_EQ(sc_agent_pool_status(p, id), SC_AGENT_IDLE);
    sc_agent_pool_destroy(p);
}

static void test_pool_cancel(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *p = sc_agent_pool_create(&a, 4);
    sc_spawn_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider = "openai"; cfg.provider_len = 6;
    uint64_t id = 0;
    sc_agent_pool_spawn(p, &cfg, "t", 1, "c", &id);
    SC_ASSERT_EQ(sc_agent_pool_cancel(p, id), SC_OK);
    SC_ASSERT_EQ(sc_agent_pool_status(p, id), SC_AGENT_CANCELLED);
    SC_ASSERT_EQ(sc_agent_pool_cancel(p, 9999), SC_ERR_NOT_FOUND);
    sc_agent_pool_destroy(p);
}

static void test_pool_list(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *p = sc_agent_pool_create(&a, 4);
    sc_spawn_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.provider = "openai"; cfg.provider_len = 6;
    uint64_t id1 = 0, id2 = 0;
    sc_agent_pool_spawn(p, &cfg, "a", 1, "l1", &id1);
    sc_agent_pool_spawn(p, &cfg, "b", 1, "l2", &id2);
    sc_agent_pool_info_t *info = NULL;
    size_t count = 0;
    SC_ASSERT_EQ(sc_agent_pool_list(p, &a, &info, &count), SC_OK);
    SC_ASSERT_EQ(count, 2);
    a.free(a.ctx, info, count * sizeof(sc_agent_pool_info_t));
    sc_agent_pool_destroy(p);
}

static void test_pool_query(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *p = sc_agent_pool_create(&a, 4);
    char *resp = NULL; size_t rlen = 0;
    SC_ASSERT_EQ(sc_agent_pool_query(p, 1, "q", 1, &resp, &rlen), SC_OK);
    SC_ASSERT_NOT_NULL(resp);
    if (resp) a.free(a.ctx, resp, rlen + 1);
    sc_agent_pool_destroy(p);
}

static void test_mailbox_create_destroy(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_mailbox_t *m = sc_mailbox_create(&a, 8);
    SC_ASSERT_NOT_NULL(m);
    sc_mailbox_destroy(m);
}

static void test_mailbox_send_recv(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_mailbox_t *m = sc_mailbox_create(&a, 8);
    SC_ASSERT_EQ(sc_mailbox_register(m, 1), SC_OK);
    SC_ASSERT_EQ(sc_mailbox_register(m, 2), SC_OK);
    SC_ASSERT_EQ(sc_mailbox_send(m, 1, 2, SC_MSG_TASK, "hello", 5, 42), SC_OK);
    SC_ASSERT_EQ(sc_mailbox_pending_count(m, 2), 1);
    sc_message_t msg;
    SC_ASSERT_EQ(sc_mailbox_recv(m, 2, &msg), SC_OK);
    SC_ASSERT_EQ(msg.type, SC_MSG_TASK);
    SC_ASSERT_EQ(msg.from_agent, 1);
    SC_ASSERT_EQ(msg.correlation_id, 42);
    SC_ASSERT_STR_EQ(msg.payload, "hello");
    sc_message_free(&a, &msg);
    SC_ASSERT_EQ(sc_mailbox_pending_count(m, 2), 0);
    sc_mailbox_destroy(m);
}

static void test_mailbox_recv_empty(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_mailbox_t *m = sc_mailbox_create(&a, 4);
    SC_ASSERT_EQ(sc_mailbox_register(m, 1), SC_OK);
    sc_message_t msg;
    SC_ASSERT_EQ(sc_mailbox_recv(m, 1, &msg), SC_ERR_NOT_FOUND);
    sc_mailbox_destroy(m);
}

static void test_mailbox_broadcast(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_mailbox_t *m = sc_mailbox_create(&a, 8);
    sc_mailbox_register(m, 1);
    sc_mailbox_register(m, 2);
    sc_mailbox_register(m, 3);
    SC_ASSERT_EQ(sc_mailbox_broadcast(m, 1, SC_MSG_CANCEL, "stop", 4), SC_OK);
    SC_ASSERT_EQ(sc_mailbox_pending_count(m, 1), 0);
    SC_ASSERT_EQ(sc_mailbox_pending_count(m, 2), 1);
    SC_ASSERT_EQ(sc_mailbox_pending_count(m, 3), 1);
    sc_message_t msg;
    sc_mailbox_recv(m, 2, &msg); sc_message_free(&a, &msg);
    sc_mailbox_recv(m, 3, &msg); sc_message_free(&a, &msg);
    sc_mailbox_destroy(m);
}

static void test_mailbox_unregister(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_mailbox_t *m = sc_mailbox_create(&a, 4);
    SC_ASSERT_EQ(sc_mailbox_register(m, 1), SC_OK);
    SC_ASSERT_EQ(sc_mailbox_send(m, 2, 1, SC_MSG_PING, "p", 1, 0), SC_OK);
    SC_ASSERT_EQ(sc_mailbox_unregister(m, 1), SC_OK);
    SC_ASSERT_EQ(sc_mailbox_send(m, 2, 1, SC_MSG_PING, "p", 1, 0), SC_ERR_NOT_FOUND);
    sc_mailbox_destroy(m);
}

static void test_thread_bind_lookup(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_thread_binding_t *tb = sc_thread_binding_create(&a, 32);
    SC_ASSERT_NOT_NULL(tb);
    SC_ASSERT_EQ(sc_thread_binding_bind(tb, "discord", "thread-123", 42, 300), SC_OK);
    uint64_t aid = 0;
    SC_ASSERT_EQ(sc_thread_binding_lookup(tb, "discord", "thread-123", &aid), SC_OK);
    SC_ASSERT_EQ(aid, 42);
    SC_ASSERT_EQ(sc_thread_binding_count(tb), 1);
    sc_thread_binding_destroy(tb);
}

static void test_thread_unbind(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_thread_binding_t *tb = sc_thread_binding_create(&a, 32);
    sc_thread_binding_bind(tb, "slack", "ts-456", 10, 0);
    SC_ASSERT_EQ(sc_thread_binding_unbind(tb, "slack", "ts-456"), SC_OK);
    uint64_t aid = 0;
    SC_ASSERT_EQ(sc_thread_binding_lookup(tb, "slack", "ts-456", &aid), SC_ERR_NOT_FOUND);
    sc_thread_binding_destroy(tb);
}

static void test_thread_expire(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_thread_binding_t *tb = sc_thread_binding_create(&a, 32);
    sc_thread_binding_bind(tb, "telegram", "chain-1", 5, 10);
    SC_ASSERT_EQ(sc_thread_binding_count(tb), 1);
    size_t expired = sc_thread_binding_expire_idle(tb, (int64_t)time(NULL) + 20);
    SC_ASSERT_EQ(expired, 1);
    SC_ASSERT_EQ(sc_thread_binding_count(tb), 0);
    sc_thread_binding_destroy(tb);
}

static void test_thread_list(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_thread_binding_t *tb = sc_thread_binding_create(&a, 32);
    sc_thread_binding_bind(tb, "discord", "a", 1, 0);
    sc_thread_binding_bind(tb, "slack", "b", 2, 0);
    sc_thread_binding_entry_t *entries = NULL;
    size_t count = 0;
    SC_ASSERT_EQ(sc_thread_binding_list(tb, &a, &entries, &count), SC_OK);
    SC_ASSERT_EQ(count, 2);
    a.free(a.ctx, entries, count * sizeof(sc_thread_binding_entry_t));
    sc_thread_binding_destroy(tb);
}

static void test_agent_spawn_tool(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *pool = sc_agent_pool_create(&a, 4);
    sc_tool_t tool = {0};
    SC_ASSERT_EQ(sc_agent_spawn_tool_create(&a, pool, &tool), SC_OK);
    SC_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "agent_spawn");
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
    sc_agent_pool_destroy(pool);
}

static void test_agent_query_tool(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *pool = sc_agent_pool_create(&a, 4);
    sc_tool_t tool = {0};
    SC_ASSERT_EQ(sc_agent_query_tool_create(&a, pool, &tool), SC_OK);
    SC_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "agent_query");
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
    sc_agent_pool_destroy(pool);
}

static void test_canvas_tool(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_tool_t tool = {0};
    SC_ASSERT_EQ(sc_canvas_create(&a, &tool), SC_OK);
    SC_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "canvas");
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void test_apply_patch_tool(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_tool_t tool = {0};
    SC_ASSERT_EQ(sc_apply_patch_create(&a, NULL, &tool), SC_OK);
    SC_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "apply_patch");
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void test_notebook_tool(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_tool_t tool = {0};
    SC_ASSERT_EQ(sc_notebook_create(&a, NULL, &tool), SC_OK);
    SC_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "notebook");
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void test_database_tool(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_tool_t tool = {0};
    SC_ASSERT_EQ(sc_database_tool_create(&a, NULL, &tool), SC_OK);
    SC_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "database");
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void test_diff_tool(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_tool_t tool = {0};
    SC_ASSERT_EQ(sc_diff_tool_create(&a, &tool), SC_OK);
    SC_ASSERT_STR_EQ(tool.vtable->name(tool.ctx), "diff");
}

static void test_policy_engine_deny(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_policy_engine_t *e = sc_policy_engine_create(&a);
    SC_ASSERT_NOT_NULL(e);
    sc_policy_match_t m = { .tool = "git", .args_contains = "push --force", .session_cost_gt = 0, .has_cost_check = false };
    SC_ASSERT_EQ(sc_policy_engine_add_rule(e, "no-force-push", m, SC_POLICY_DENY, "blocked"), SC_OK);
    sc_policy_eval_ctx_t ctx = { .tool_name = "git", .args_json = "{\"cmd\":\"push --force\"}", .session_cost_usd = 0 };
    SC_ASSERT_EQ(sc_policy_engine_evaluate(e, &ctx).action, SC_POLICY_DENY);
    sc_policy_eval_ctx_t ctx2 = { .tool_name = "git", .args_json = "{\"cmd\":\"push\"}", .session_cost_usd = 0 };
    SC_ASSERT_EQ(sc_policy_engine_evaluate(e, &ctx2).action, SC_POLICY_ALLOW);
    sc_policy_engine_destroy(e);
}

static void test_policy_engine_cost(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_policy_engine_t *e = sc_policy_engine_create(&a);
    sc_policy_match_t m = { .tool = NULL, .args_contains = NULL, .session_cost_gt = 5.0, .has_cost_check = true };
    sc_policy_engine_add_rule(e, "budget", m, SC_POLICY_DENY, "over budget");
    sc_policy_eval_ctx_t c1 = { .tool_name = "shell", .args_json = NULL, .session_cost_usd = 3.0 };
    SC_ASSERT_EQ(sc_policy_engine_evaluate(e, &c1).action, SC_POLICY_ALLOW);
    sc_policy_eval_ctx_t c2 = { .tool_name = "shell", .args_json = NULL, .session_cost_usd = 6.0 };
    SC_ASSERT_EQ(sc_policy_engine_evaluate(e, &c2).action, SC_POLICY_DENY);
    sc_policy_engine_destroy(e);
}

static void test_policy_engine_approval(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_policy_engine_t *e = sc_policy_engine_create(&a);
    sc_policy_match_t m = { .tool = "shell", .args_contains = "rm -rf", .session_cost_gt = 0, .has_cost_check = false };
    sc_policy_engine_add_rule(e, "approve-rm", m, SC_POLICY_REQUIRE_APPROVAL, "needs approval");
    sc_policy_eval_ctx_t c = { .tool_name = "shell", .args_json = "rm -rf /tmp", .session_cost_usd = 0 };
    SC_ASSERT_EQ(sc_policy_engine_evaluate(e, &c).action, SC_POLICY_REQUIRE_APPROVAL);
    sc_policy_engine_destroy(e);
}

static void test_otel_observer(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_observer_t obs = {0};
    sc_otel_config_t cfg = { .endpoint = "http://localhost:4317", .endpoint_len = 21,
        .service_name = "seaclaw", .service_name_len = 7,
        .enable_traces = true, .enable_metrics = true, .enable_logs = true };
    SC_ASSERT_EQ(sc_otel_observer_create(&a, &cfg, &obs), SC_OK);
    SC_ASSERT_STR_EQ(obs.vtable->name(obs.ctx), "otel");
    if (obs.vtable->deinit) obs.vtable->deinit(obs.ctx);
}

static void test_otel_span(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_span_t *span = sc_span_start(&a, "test_span", 9);
    SC_ASSERT_NOT_NULL(span);
    sc_span_set_attr_str(span, "tool", "shell");
    sc_span_set_attr_int(span, "tokens", 100);
    sc_span_set_attr_double(span, "cost", 0.05);
    SC_ASSERT_EQ(sc_span_end(span, &a), SC_OK);
}

static void test_replay_record(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_replay_recorder_t *r = sc_replay_recorder_create(&a, 100);
    SC_ASSERT_NOT_NULL(r);
    SC_ASSERT_EQ(sc_replay_record(r, SC_REPLAY_TOOL_CALL, "{\"tool\":\"shell\"}", 16), SC_OK);
    SC_ASSERT_EQ(sc_replay_record(r, SC_REPLAY_TOOL_RESULT, "ok", 2), SC_OK);
    SC_ASSERT_EQ(sc_replay_event_count(r), 2);
    sc_replay_event_t ev;
    SC_ASSERT_EQ(sc_replay_get_event(r, 0, &ev), SC_OK);
    SC_ASSERT_EQ(ev.type, SC_REPLAY_TOOL_CALL);
    char *json = NULL; size_t jlen = 0;
    SC_ASSERT_EQ(sc_replay_export_json(r, &a, &json, &jlen), SC_OK);
    SC_ASSERT_NOT_NULL(json);
    a.free(a.ctx, json, jlen + 1);
    sc_replay_recorder_destroy(r);
}

static void test_plugin_registry(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_plugin_registry_t *reg = sc_plugin_registry_create(&a, 16);
    SC_ASSERT_NOT_NULL(reg);
    sc_plugin_info_t info = { .name = "test-plugin", .version = "1.0.0",
        .description = "A test plugin", .api_version = SC_PLUGIN_API_VERSION };
    SC_ASSERT_EQ(sc_plugin_register(reg, &info, NULL, 0), SC_OK);
    SC_ASSERT_EQ(sc_plugin_count(reg), 1);
    sc_plugin_info_t out;
    SC_ASSERT_EQ(sc_plugin_get_info(reg, 0, &out), SC_OK);
    SC_ASSERT_STR_EQ(out.name, "test-plugin");
    sc_plugin_registry_destroy(reg);
}

static void test_plugin_bad_version(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_plugin_registry_t *reg = sc_plugin_registry_create(&a, 4);
    sc_plugin_info_t info = { .name = "bad", .version = "0.0.1", .description = NULL, .api_version = 999 };
    SC_ASSERT_EQ(sc_plugin_register(reg, &info, NULL, 0), SC_ERR_INVALID_ARGUMENT);
    sc_plugin_registry_destroy(reg);
}

static void test_profile_get_coding(void) {
    const sc_agent_profile_t *p = sc_agent_profile_get(SC_PROFILE_CODING);
    SC_ASSERT_NOT_NULL(p);
    SC_ASSERT_STR_EQ(p->name, "coding");
    SC_ASSERT(p->default_tools_count > 0);
    SC_ASSERT_EQ(p->autonomy_level, 2);
}

static void test_profile_get_ops(void) {
    const sc_agent_profile_t *p = sc_agent_profile_get(SC_PROFILE_OPS);
    SC_ASSERT_NOT_NULL(p);
    SC_ASSERT_STR_EQ(p->name, "ops");
}

static void test_profile_get_messaging(void) {
    const sc_agent_profile_t *p = sc_agent_profile_get(SC_PROFILE_MESSAGING);
    SC_ASSERT_NOT_NULL(p);
    SC_ASSERT_STR_EQ(p->name, "messaging");
}

static void test_profile_get_minimal(void) {
    const sc_agent_profile_t *p = sc_agent_profile_get(SC_PROFILE_MINIMAL);
    SC_ASSERT_NOT_NULL(p);
    SC_ASSERT_STR_EQ(p->name, "minimal");
    SC_ASSERT_EQ(p->autonomy_level, 0);
}

static void test_profile_by_name(void) {
    const sc_agent_profile_t *p = sc_agent_profile_by_name("coding", 6);
    SC_ASSERT_NOT_NULL(p);
    SC_ASSERT_EQ(p->type, SC_PROFILE_CODING);
    SC_ASSERT_NULL(sc_agent_profile_by_name("nonexist", 8));
}

static void test_profile_list(void) {
    const sc_agent_profile_t *profiles = NULL;
    size_t count = 0;
    SC_ASSERT_EQ(sc_agent_profile_list(&profiles, &count), SC_OK);
    SC_ASSERT(count >= 4);
    SC_ASSERT_NOT_NULL(profiles);
}

static void test_pool_null_alloc(void) {
    SC_ASSERT_NULL(sc_agent_pool_create(NULL, 4));
}

static void test_pool_zero_max(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *pool = sc_agent_pool_create(&a, 0);
    SC_ASSERT_NOT_NULL(pool);
    SC_ASSERT_EQ(sc_agent_pool_running_count(pool), 0);
    sc_agent_pool_destroy(pool);
}

static void test_pool_status_invalid_id(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *pool = sc_agent_pool_create(&a, 2);
    SC_ASSERT_EQ(sc_agent_pool_status(pool, 99999), SC_AGENT_FAILED);
    sc_agent_pool_destroy(pool);
}

static void test_pool_cancel_invalid_id(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *pool = sc_agent_pool_create(&a, 2);
    SC_ASSERT_EQ(sc_agent_pool_cancel(pool, 99999), SC_ERR_NOT_FOUND);
    sc_agent_pool_destroy(pool);
}

static void test_pool_result_invalid_id(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_agent_pool_t *pool = sc_agent_pool_create(&a, 2);
    SC_ASSERT_NULL(sc_agent_pool_result(pool, 99999));
    sc_agent_pool_destroy(pool);
}

static void test_canvas_description(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_tool_t tool = {0};
    SC_ASSERT_EQ(sc_canvas_create(&a, &tool), SC_OK);
    SC_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    SC_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void test_apply_patch_description(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_tool_t tool = {0};
    SC_ASSERT_EQ(sc_apply_patch_create(&a, NULL, &tool), SC_OK);
    SC_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    SC_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void test_notebook_description(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_tool_t tool = {0};
    SC_ASSERT_EQ(sc_notebook_create(&a, NULL, &tool), SC_OK);
    SC_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    SC_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void test_diff_description(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_tool_t tool = {0};
    SC_ASSERT_EQ(sc_diff_tool_create(&a, &tool), SC_OK);
    SC_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    SC_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
}

static void test_database_description(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_tool_t tool = {0};
    SC_ASSERT_EQ(sc_database_tool_create(&a, NULL, &tool), SC_OK);
    SC_ASSERT_NOT_NULL(tool.vtable->description(tool.ctx));
    SC_ASSERT_NOT_NULL(tool.vtable->parameters_json(tool.ctx));
    if (tool.vtable->deinit) tool.vtable->deinit(tool.ctx, &a);
}

static void test_policy_engine_null(void) {
    SC_ASSERT_NULL(sc_policy_engine_create(NULL));
}

static void test_policy_engine_no_rules_allows(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_policy_engine_t *e = sc_policy_engine_create(&a);
    sc_policy_eval_ctx_t ctx = { .tool_name = "shell", .args_json = NULL, .session_cost_usd = 0 };
    SC_ASSERT_EQ(sc_policy_engine_evaluate(e, &ctx).action, SC_POLICY_ALLOW);
    sc_policy_engine_destroy(e);
}

static void test_mailbox_send_unregistered_target(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_mailbox_t *mb = sc_mailbox_create(&a, 4);
    SC_ASSERT_EQ(sc_mailbox_send(mb, 0, 99, SC_MSG_TASK, "hi", 2, 0), SC_ERR_NOT_FOUND);
    sc_mailbox_destroy(mb);
}

static void test_profile_by_name_null(void) {
    SC_ASSERT_NULL(sc_agent_profile_by_name(NULL, 0));
    SC_ASSERT_NULL(sc_agent_profile_by_name("", 0));
}


static void test_websocket_wss_not_supported(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_websocket_t ws;
    memset(&ws, 0, sizeof(ws));
    sc_error_t err = sc_websocket_connect(&ws, &a, "wss://example.com/ws");
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
}

static void test_voice_tts_not_supported(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_voice_config_t vc;
    memset(&vc, 0, sizeof(vc));
    sc_error_t err = sc_voice_tts_synthesize(&a, &vc, "hello", 5, NULL, NULL);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
}

static void test_voice_stt_not_supported(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_voice_config_t vc;
    memset(&vc, 0, sizeof(vc));
    sc_error_t err = sc_voice_stt_transcribe(&a, &vc, NULL, 0, NULL, NULL);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
}

static void test_multimodal_image_analyze_not_supported(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_error_t err = sc_multimodal_analyze_image(&a, NULL, NULL, 0, NULL, NULL);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
}

static void test_daemon_start_not_supported(void) {
    sc_error_t err = sc_daemon_start(NULL);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
}

static void test_update_check_not_supported(void) {
    sc_allocator_t a = sc_system_allocator();
    sc_error_t err = sc_update_check(&a, NULL, NULL);
    SC_ASSERT_EQ(err, SC_ERR_NOT_SUPPORTED);
}

void run_roadmap_tests(void) {
    SC_TEST_SUITE("Roadmap: Agent Pool (1A)");
    SC_RUN_TEST(test_pool_create_destroy);
    SC_RUN_TEST(test_pool_spawn_one_shot);
    SC_RUN_TEST(test_pool_spawn_persistent);
    SC_RUN_TEST(test_pool_cancel);
    SC_RUN_TEST(test_pool_list);
    SC_RUN_TEST(test_pool_query);
    SC_RUN_TEST(test_pool_null_alloc);
    SC_RUN_TEST(test_pool_zero_max);
    SC_RUN_TEST(test_pool_status_invalid_id);
    SC_RUN_TEST(test_pool_cancel_invalid_id);
    SC_RUN_TEST(test_pool_result_invalid_id);

    SC_TEST_SUITE("Roadmap: Mailbox (1B)");
    SC_RUN_TEST(test_mailbox_create_destroy);
    SC_RUN_TEST(test_mailbox_send_recv);
    SC_RUN_TEST(test_mailbox_recv_empty);
    SC_RUN_TEST(test_mailbox_broadcast);
    SC_RUN_TEST(test_mailbox_unregister);
    SC_RUN_TEST(test_mailbox_send_unregistered_target);

    SC_TEST_SUITE("Roadmap: Thread Binding (1C)");
    SC_RUN_TEST(test_thread_bind_lookup);
    SC_RUN_TEST(test_thread_unbind);
    SC_RUN_TEST(test_thread_expire);
    SC_RUN_TEST(test_thread_list);

    SC_TEST_SUITE("Roadmap: Agent Tools (1D+E)");
    SC_RUN_TEST(test_agent_spawn_tool);
    SC_RUN_TEST(test_agent_query_tool);

    SC_TEST_SUITE("Roadmap: Agent Profiles (2)");
    SC_RUN_TEST(test_profile_get_coding);
    SC_RUN_TEST(test_profile_get_ops);
    SC_RUN_TEST(test_profile_get_messaging);
    SC_RUN_TEST(test_profile_get_minimal);
    SC_RUN_TEST(test_profile_by_name);
    SC_RUN_TEST(test_profile_by_name_null);
    SC_RUN_TEST(test_profile_list);

    SC_TEST_SUITE("Roadmap: Power Tools (3)");
    SC_RUN_TEST(test_canvas_tool);
    SC_RUN_TEST(test_canvas_description);
    SC_RUN_TEST(test_apply_patch_tool);
    SC_RUN_TEST(test_apply_patch_description);
    SC_RUN_TEST(test_notebook_tool);
    SC_RUN_TEST(test_notebook_description);
    SC_RUN_TEST(test_database_tool);
    SC_RUN_TEST(test_database_description);
    SC_RUN_TEST(test_diff_tool);
    SC_RUN_TEST(test_diff_description);

    SC_TEST_SUITE("Roadmap: Policy Engine (4A)");
    SC_RUN_TEST(test_policy_engine_deny);
    SC_RUN_TEST(test_policy_engine_cost);
    SC_RUN_TEST(test_policy_engine_approval);
    SC_RUN_TEST(test_policy_engine_null);
    SC_RUN_TEST(test_policy_engine_no_rules_allows);

    SC_TEST_SUITE("Roadmap: OTel + Tracing (4B)");
    SC_RUN_TEST(test_otel_observer);
    SC_RUN_TEST(test_otel_span);

    SC_TEST_SUITE("Roadmap: Action Replay (4D)");
    SC_RUN_TEST(test_replay_record);

    SC_TEST_SUITE("Roadmap: Plugin System (5B)");
    SC_RUN_TEST(test_plugin_registry);
    SC_RUN_TEST(test_plugin_bad_version);

    SC_TEST_SUITE("Roadmap: Stub Boundaries (6)");
    SC_RUN_TEST(test_daemon_start_not_supported);
    SC_RUN_TEST(test_update_check_not_supported);
}
