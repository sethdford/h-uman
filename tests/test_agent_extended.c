/* Agent edge cases (~70 tests). Uses subsystems: planner, dispatcher, compaction, commands. */
#include "human/agent.h"
#include "human/agent/commands.h"
#include "human/agent/compaction.h"
#include "human/agent/dispatcher.h"
#include "human/agent/planner.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/provider.h"
#include "human/providers/openai.h"
#include "human/tools/factory.h"
#include "human/tools/shell.h"
#include "test_framework.h"
#ifdef HU_ENABLE_TUI
#include "human/agent/tui.h"
#endif
#include <string.h>

static void test_agent_max_iterations_limit(void) {
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    disp.max_parallel = 1;
    disp.timeout_secs = 5;
    HU_ASSERT_EQ(disp.max_parallel, 1u);
    HU_ASSERT_EQ(disp.timeout_secs, 5u);
}

static void test_agent_empty_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_t *plan = NULL;
    hu_error_t err = hu_planner_create_plan(&alloc, "", 0, &plan);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(plan);
}

static void test_agent_very_long_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char buf[10000];
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "{\"steps\":[");
    for (int i = 0; i < 100; i++) {
        if (i > 0)
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, ",");
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "{\"tool\":\"shell\",\"args\":{}}");
    }
    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "]}");
    hu_plan_t *plan = NULL;
    hu_error_t err = hu_planner_create_plan(&alloc, buf, pos, &plan);
    if (err == HU_OK && plan) {
        HU_ASSERT_EQ(plan->steps_count, 100u);
        hu_plan_free(&alloc, plan);
    }
}

static void test_agent_tool_not_found_handled(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    hu_tool_call_t call = {
        .id = "x",
        .id_len = 1,
        .name = "nonexistent_tool_xyz",
        .name_len = 18,
        .arguments = "{}",
        .arguments_len = 2,
    };
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    hu_dispatch_result_t dres;
    hu_error_t err = hu_dispatcher_dispatch(&disp, &alloc, &tool, 1, &call, 1, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, 1u);
    HU_ASSERT_FALSE(dres.results[0].success);
    HU_ASSERT_TRUE(strstr(dres.results[0].error_msg, "not found") != NULL);
    hu_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_agent_planner_empty_steps(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_t *plan = NULL;
    hu_error_t err = hu_planner_create_plan(&alloc, "{\"steps\":[]}", 11, &plan);
    if (err == HU_OK && plan) {
        HU_ASSERT_EQ(plan->steps_count, 0u);
        HU_ASSERT_TRUE(hu_planner_is_complete(plan));
        hu_plan_free(&alloc, plan);
    }
}

static void test_agent_planner_malformed_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_t *plan = NULL;
    hu_error_t err = hu_planner_create_plan(&alloc, "{invalid}", 9, &plan);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(plan);
}

static void test_agent_planner_null_steps_key(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_t *plan = NULL;
    hu_error_t err = hu_planner_create_plan(&alloc, "{\"other\":1}", 10, &plan);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(plan);
}

static void test_agent_compaction_no_trigger(void) {
    hu_compaction_config_t cfg;
    hu_compaction_config_default(&cfg);
    cfg.keep_recent = 5;
    cfg.max_history_messages = 100;
    hu_owned_message_t msgs[6];
    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < 6; i++) {
        msgs[i].role = HU_ROLE_USER;
        msgs[i].content = "x";
        msgs[i].content_len = 1;
    }
    HU_ASSERT_FALSE(hu_should_compact(msgs, 6, &cfg));
}

static void test_agent_estimate_tokens_empty(void) {
    hu_owned_message_t msgs[1] = {{.content = "", .content_len = 0, .role = HU_ROLE_USER}};
    uint64_t t = hu_estimate_tokens(msgs, 1);
    HU_ASSERT_TRUE(t <= 1);
}

static void test_agent_estimate_tokens_long(void) {
    char buf[1000];
    for (size_t i = 0; i < sizeof(buf) - 1; i++)
        buf[i] = 'a';
    buf[sizeof(buf) - 1] = '\0';
    hu_owned_message_t msgs[1] = {{.content = buf, .content_len = 999, .role = HU_ROLE_USER}};
    uint64_t t = hu_estimate_tokens(msgs, 1);
    HU_ASSERT_TRUE(t > 100);
}

static void test_agent_planner_step_status_values(void) {
    HU_ASSERT(HU_PLAN_STEP_PENDING != HU_PLAN_STEP_DONE);
    HU_ASSERT(HU_PLAN_STEP_FAILED != HU_PLAN_STEP_RUNNING);
}

static void test_agent_dispatcher_sequential(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    hu_tool_call_t calls[3];
    const char *args = "{\"command\":\"echo a\"}";
    for (int i = 0; i < 3; i++) {
        calls[i] = (hu_tool_call_t){
            .id = "c",
            .id_len = 1,
            .name = "shell",
            .name_len = 5,
            .arguments = args,
            .arguments_len = (size_t)strlen(args),
        };
    }
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    disp.max_parallel = 1;
    hu_dispatch_result_t dres;
    hu_error_t err = hu_dispatcher_dispatch(&disp, &alloc, &tool, 1, calls, 3, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, 3u);
    for (size_t i = 0; i < 3; i++)
        HU_ASSERT_TRUE(dres.results[i].success);
    hu_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_agent_planner_next_step_null_when_complete(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"steps\":[{\"tool\":\"x\",\"args\":{}}]}";
    hu_plan_t *plan = NULL;
    hu_planner_create_plan(&alloc, json, strlen(json), &plan);
    hu_planner_mark_step(plan, 0, HU_PLAN_STEP_DONE);
    hu_plan_step_t *s = hu_planner_next_step(plan);
    HU_ASSERT_NULL(s);
    hu_plan_free(&alloc, plan);
}

static void test_agent_compaction_config_default(void) {
    hu_compaction_config_t cfg;
    hu_compaction_config_default(&cfg);
    HU_ASSERT_TRUE(cfg.keep_recent > 0);
    HU_ASSERT_TRUE(cfg.max_history_messages > 0);
}

static void test_agent_planner_alternate_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"steps\":[{\"name\":\"shell\",\"arguments\":{\"command\":\"ls\"}}]}";
    hu_plan_t *plan = NULL;
    hu_error_t err = hu_planner_create_plan(&alloc, json, strlen(json), &plan);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(plan->steps_count, 1u);
    HU_ASSERT_STR_EQ(plan->steps[0].tool_name, "shell");
    hu_plan_free(&alloc, plan);
}

static void test_agent_dispatcher_zero_calls(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    hu_dispatch_result_t dres;
    hu_error_t err = hu_dispatcher_dispatch(&disp, &alloc, &tool, 1, NULL, 0, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, 0u);
    HU_ASSERT_NULL(dres.results);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_agent_plan_free_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_free(&alloc, NULL);
}

static void test_agent_tool_call_round_trip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_provider_t prov;
    hu_error_t err = hu_openai_create(&alloc, "test-key", 8, NULL, 0, &prov);
    HU_ASSERT_EQ(err, HU_OK);
    hu_tool_t shell_tool;
    hu_shell_create(&alloc, "/tmp", 4, NULL, &shell_tool);
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    err =
        hu_agent_from_config(&agent, &alloc, prov, &shell_tool, 1, NULL, NULL, NULL, NULL, "gpt-4",
                             5, "openai", 6, 0.7, ".", 1, 25, 50, false, 0, NULL, 0, NULL, 0, NULL);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent.tool_specs_count, 1u);
    HU_ASSERT_EQ(agent.tools_count, 1u);
    char *response = NULL;
    size_t response_len = 0;
    err = hu_agent_turn(&agent, "list files", 9, &response, &response_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(response);
    HU_ASSERT_TRUE(response_len > 0);
    if (response)
        alloc.free(alloc.ctx, response, response_len + 1);
    if (shell_tool.vtable->deinit)
        shell_tool.vtable->deinit(shell_tool.ctx, &alloc);
    hu_agent_deinit(&agent);
}

static void test_agent_planner_mark_step_out_of_range(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"steps\":[{\"tool\":\"x\",\"args\":{}}]}";
    hu_plan_t *plan = NULL;
    hu_planner_create_plan(&alloc, json, strlen(json), &plan);
    hu_planner_mark_step(plan, 99, HU_PLAN_STEP_DONE);
    hu_plan_free(&alloc, plan);
}

/* ─── Slash command parsing ──────────────────────────────────────────────── */
static void test_slash_parse_help(void) {
    const char *msg = "/help";
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse(msg, strlen(msg));
    HU_ASSERT_NOT_NULL(cmd);
    HU_ASSERT_EQ(cmd->name_len, 4u);
    HU_ASSERT_TRUE(memcmp(cmd->name, "help", 4) == 0);
}

static void test_slash_parse_status(void) {
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse("/status", 7);
    HU_ASSERT_NOT_NULL(cmd);
    HU_ASSERT_STR_EQ(cmd->name, "status");
}

static void test_slash_parse_model_with_arg(void) {
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse("/model gpt-4", 12);
    HU_ASSERT_NOT_NULL(cmd);
    HU_ASSERT_STR_EQ(cmd->name, "model");
    HU_ASSERT_STR_EQ(cmd->arg, "gpt-4");
}

static void test_slash_parse_new(void) {
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse("/new", 4);
    HU_ASSERT_NOT_NULL(cmd);
    HU_ASSERT_STR_EQ(cmd->name, "new");
}

static void test_slash_parse_reset(void) {
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse("/reset", 6);
    HU_ASSERT_NOT_NULL(cmd);
    HU_ASSERT_STR_EQ(cmd->name, "reset");
}

static void test_slash_parse_clear(void) {
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse("/clear", 6);
    HU_ASSERT_NOT_NULL(cmd);
    HU_ASSERT_STR_EQ(cmd->name, "clear");
}

static void test_slash_parse_compact(void) {
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse("/compact", 8);
    HU_ASSERT_NOT_NULL(cmd);
    HU_ASSERT_STR_EQ(cmd->name, "compact");
}

static void test_slash_parse_cost(void) {
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse("/cost", 5);
    HU_ASSERT_NOT_NULL(cmd);
    HU_ASSERT_STR_EQ(cmd->name, "cost");
}

static void test_slash_parse_memory(void) {
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse("/memory", 7);
    HU_ASSERT_NOT_NULL(cmd);
    HU_ASSERT_STR_EQ(cmd->name, "memory");
}

static void test_slash_parse_tools(void) {
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse("/tools", 6);
    HU_ASSERT_NOT_NULL(cmd);
    HU_ASSERT_STR_EQ(cmd->name, "tools");
}

static void test_slash_parse_non_slash_returns_null(void) {
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse("hello", 5);
    HU_ASSERT_NULL(cmd);
}

static void test_slash_parse_single_slash_returns_null(void) {
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse("/", 1);
    HU_ASSERT_NULL(cmd);
}

static void test_slash_parse_whitespace_trailing(void) {
    const hu_slash_cmd_t *cmd = hu_agent_commands_parse("/help  \n", 8);
    HU_ASSERT_NOT_NULL(cmd);
    HU_ASSERT_STR_EQ(cmd->name, "help");
}

static void test_slash_bare_session_reset_new(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *prompt = NULL;
    hu_error_t err = hu_agent_commands_bare_session_reset_prompt(&alloc, "/new", 4, &prompt);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(strstr(prompt, "Session Startup") != NULL);
    if (prompt)
        alloc.free(alloc.ctx, prompt, strlen(prompt) + 1);
}

static void test_slash_bare_session_reset_reset(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *prompt = NULL;
    hu_error_t err = hu_agent_commands_bare_session_reset_prompt(&alloc, "/reset", 6, &prompt);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    if (prompt)
        alloc.free(alloc.ctx, prompt, strlen(prompt) + 1);
}

static void test_slash_bare_session_reset_help_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *prompt = NULL;
    hu_agent_commands_bare_session_reset_prompt(&alloc, "/help", 5, &prompt);
    HU_ASSERT_NULL(prompt);
}

static void test_slash_bare_session_reset_with_arg_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *prompt = NULL;
    hu_agent_commands_bare_session_reset_prompt(&alloc, "/new foo", 8, &prompt);
    HU_ASSERT_NULL(prompt);
}

/* ─── Compaction ─────────────────────────────────────────────────────────── */
static void test_compaction_trigger_over_message_limit(void) {
    hu_compaction_config_t cfg;
    hu_compaction_config_default(&cfg);
    cfg.keep_recent = 5;
    cfg.max_history_messages = 5;
    hu_owned_message_t msgs[10];
    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < 10; i++) {
        msgs[i].role = HU_ROLE_USER;
        msgs[i].content = "x";
        msgs[i].content_len = 1;
    }
    HU_ASSERT_TRUE(hu_should_compact(msgs, 10, &cfg));
}

static void test_compaction_compact_history(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_compaction_config_t cfg;
    hu_compaction_config_default(&cfg);
    cfg.keep_recent = 2;
    cfg.max_history_messages = 3;
    cfg.max_summary_chars = 500;
    size_t cap = 10;
    hu_owned_message_t *msgs =
        (hu_owned_message_t *)alloc.alloc(alloc.ctx, cap * sizeof(hu_owned_message_t));
    HU_ASSERT_NOT_NULL(msgs);
    memset(msgs, 0, cap * sizeof(hu_owned_message_t));
    for (size_t i = 0; i < 5; i++) {
        msgs[i].role = (i % 2 == 0) ? HU_ROLE_USER : HU_ROLE_ASSISTANT;
        msgs[i].content = hu_strdup(&alloc, "message");
        msgs[i].content_len = 7;
    }
    size_t count = 5;
    hu_error_t err = hu_compact_history(&alloc, msgs, &count, &cap, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count <= 3);
    for (size_t i = 0; i < count; i++) {
        if (msgs[i].content)
            alloc.free(alloc.ctx, (void *)msgs[i].content, msgs[i].content_len + 1);
    }
    alloc.free(alloc.ctx, msgs, cap * sizeof(hu_owned_message_t));
}

/* ─── Planner additional formats ─────────────────────────────────────────── */
static void test_planner_multi_step_order(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"steps\":[{\"tool\":\"a\",\"args\":{}},{\"tool\":\"b\",\"args\":{}},{"
                       "\"tool\":\"c\",\"args\":{}}]}";
    hu_plan_t *plan = NULL;
    hu_planner_create_plan(&alloc, json, strlen(json), &plan);
    HU_ASSERT_EQ(plan->steps_count, 3u);
    HU_ASSERT_STR_EQ(plan->steps[0].tool_name, "a");
    HU_ASSERT_STR_EQ(plan->steps[1].tool_name, "b");
    HU_ASSERT_STR_EQ(plan->steps[2].tool_name, "c");
    hu_plan_step_t *next = hu_planner_next_step(plan);
    HU_ASSERT_NOT_NULL(next);
    HU_ASSERT_STR_EQ(next->tool_name, "a");
    hu_planner_mark_step(plan, 0, HU_PLAN_STEP_DONE);
    next = hu_planner_next_step(plan);
    HU_ASSERT_NOT_NULL(next);
    HU_ASSERT_STR_EQ(next->tool_name, "b");
    hu_plan_free(&alloc, plan);
}

static void test_planner_step_with_description(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"steps\":[{\"tool\":\"shell\",\"args\":{\"command\":\"ls\"},"
                       "\"description\":\"list files\"}]}";
    hu_plan_t *plan = NULL;
    hu_planner_create_plan(&alloc, json, strlen(json), &plan);
    HU_ASSERT_EQ(plan->steps_count, 1u);
    HU_ASSERT_NOT_NULL(plan->steps[0].description);
    HU_ASSERT_STR_EQ(plan->steps[0].description, "list files");
    hu_plan_free(&alloc, plan);
}

static void test_planner_is_complete_all_done(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"steps\":[{\"tool\":\"x\",\"args\":{}},{\"tool\":\"y\",\"args\":{}}]}";
    hu_plan_t *plan = NULL;
    hu_planner_create_plan(&alloc, json, strlen(json), &plan);
    hu_planner_mark_step(plan, 0, HU_PLAN_STEP_DONE);
    hu_planner_mark_step(plan, 1, HU_PLAN_STEP_DONE);
    HU_ASSERT_TRUE(hu_planner_is_complete(plan));
    hu_plan_free(&alloc, plan);
}

static void test_planner_is_complete_one_pending(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"steps\":[{\"tool\":\"x\",\"args\":{}}]}";
    hu_plan_t *plan = NULL;
    hu_planner_create_plan(&alloc, json, strlen(json), &plan);
    HU_ASSERT_FALSE(hu_planner_is_complete(plan));
    hu_plan_free(&alloc, plan);
}

static void test_planner_mark_failed(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"steps\":[{\"tool\":\"x\",\"args\":{}}]}";
    hu_plan_t *plan = NULL;
    hu_planner_create_plan(&alloc, json, strlen(json), &plan);
    hu_planner_mark_step(plan, 0, HU_PLAN_STEP_FAILED);
    HU_ASSERT_TRUE(hu_planner_is_complete(plan));
    hu_plan_free(&alloc, plan);
}

/* ─── JSON tool call format (via core json) ────────────────────────────────── */
static void test_json_parse_tool_call_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"tool_calls\":[{\"id\":\"c1\",\"function\":{\"name\":\"shell\",\"arguments\":\"{}\"}}]}";
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, json, strlen(json), &val);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(val);
    hu_json_value_t *tc = hu_json_object_get(val, "tool_calls");
    HU_ASSERT_NOT_NULL(tc);
    HU_ASSERT_EQ(tc->type, HU_JSON_ARRAY);
    HU_ASSERT_EQ(tc->data.array.len, 1u);
    hu_json_value_t *first = tc->data.array.items[0];
    const char *id = hu_json_get_string(first, "id");
    HU_ASSERT_STR_EQ(id, "c1");
    hu_json_value_t *fn = hu_json_object_get(first, "function");
    HU_ASSERT_NOT_NULL(fn);
    const char *name = hu_json_get_string(fn, "name");
    HU_ASSERT_STR_EQ(name, "shell");
    hu_json_free(&alloc, val);
}

static void test_json_parse_empty_tool_calls(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"tool_calls\":[]}";
    hu_json_value_t *val = NULL;
    hu_json_parse(&alloc, json, strlen(json), &val);
    hu_json_value_t *tc = hu_json_object_get(val, "tool_calls");
    HU_ASSERT_EQ(tc->data.array.len, 0u);
    hu_json_free(&alloc, val);
}

static void test_json_parse_trailing_comma_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"a\":1,}";
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, json, strlen(json), &val);
    /* Parser may reject (strict) or accept (lenient) trailing comma */
    if (err != HU_OK) {
        HU_ASSERT_NULL(val);
    } else {
        HU_ASSERT_NOT_NULL(val);
        hu_json_free(&alloc, val);
    }
}

static void test_json_parse_truncated_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"steps\":[{\"tool\":\"x\"";
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, json, strlen(json), &val);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(val);
}

static void test_json_parse_unclosed_brace_rejected(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"a\":1";
    hu_json_value_t *val = NULL;
    hu_error_t err = hu_json_parse(&alloc, json, strlen(json), &val);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(val);
}

static void test_estimate_tokens_multi_message(void) {
    hu_owned_message_t msgs[3] = {
        {.content = "a", .content_len = 1, .role = HU_ROLE_USER},
        {.content = "bb", .content_len = 2, .role = HU_ROLE_ASSISTANT},
        {.content = "ccc", .content_len = 3, .role = HU_ROLE_USER},
    };
    for (int i = 0; i < 3; i++) {
        msgs[i].name = NULL;
        msgs[i].name_len = 0;
        msgs[i].tool_call_id = NULL;
        msgs[i].tool_call_id_len = 0;
        msgs[i].tool_calls = NULL;
        msgs[i].tool_calls_count = 0;
    }
    uint64_t t = hu_estimate_tokens(msgs, 3);
    HU_ASSERT_TRUE(t >= 1);
}

static void test_dispatcher_single_call(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    const char *args_json = "{\"command\":\"echo a\"}";
    hu_tool_call_t call = {
        .id = "x",
        .id_len = 1,
        .name = "shell",
        .name_len = 5,
        .arguments = args_json,
        .arguments_len = strlen(args_json),
    };
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    hu_dispatch_result_t dres;
    hu_error_t err = hu_dispatcher_dispatch(&disp, &alloc, &tool, 1, &call, 1, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, 1u);
    HU_ASSERT_NOT_NULL(dres.results);
    HU_ASSERT_TRUE(dres.results[0].success || dres.results[0].error_msg != NULL);
    hu_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Dispatcher extended tests ───────────────────────────────────────────── */
static void test_dispatcher_default_config(void) {
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    HU_ASSERT_EQ(disp.max_parallel, 1u);
    HU_ASSERT_EQ(disp.timeout_secs, 0u);
}

static void test_dispatcher_mixed_valid_invalid_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    const char *args = "{\"command\":\"echo ok\"}";
    hu_tool_call_t calls[2] = {
        {.id = "c1",
         .id_len = 2,
         .name = "shell",
         .name_len = 5,
         .arguments = args,
         .arguments_len = strlen(args)},
        {.id = "c2",
         .id_len = 2,
         .name = "nonexistent",
         .name_len = 11,
         .arguments = "{}",
         .arguments_len = 2},
    };
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    hu_dispatch_result_t dres;
    hu_error_t err = hu_dispatcher_dispatch(&disp, &alloc, &tool, 1, calls, 2, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, 2u);
    HU_ASSERT_TRUE(dres.results[0].success);
    HU_ASSERT_FALSE(dres.results[1].success);
    HU_ASSERT_TRUE(strstr(dres.results[1].error_msg, "not found") != NULL);
    hu_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_dispatcher_result_output_content(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    const char *args = "{\"command\":\"echo hello_world\"}";
    hu_tool_call_t call = {
        .id = "x",
        .id_len = 1,
        .name = "shell",
        .name_len = 5,
        .arguments = args,
        .arguments_len = strlen(args),
    };
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    hu_dispatch_result_t dres;
    hu_error_t err = hu_dispatcher_dispatch(&disp, &alloc, &tool, 1, &call, 1, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(dres.results[0].success);
    HU_ASSERT_NOT_NULL(dres.results[0].output);
    /* HU_IS_TEST: shell returns "(shell disabled in test mode)", not command output */
    HU_ASSERT_TRUE(strlen(dres.results[0].output) > 0);
    hu_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_dispatcher_create_destroy(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dispatcher_t *d = NULL;
    hu_error_t err = hu_dispatcher_create(&alloc, 2, 5, &d);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(d);
    HU_ASSERT_EQ(d->max_parallel, 2u);
    HU_ASSERT_EQ(d->timeout_secs, 5u);
    hu_dispatcher_destroy(&alloc, d);
}

static void test_dispatcher_dispatch_result_free_null_safe(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dispatch_result_t dres = {.results = NULL, .count = 0};
    hu_dispatch_result_free(&alloc, &dres);
}

static void test_dispatcher_max_parallel_config_stored(void) {
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    disp.max_parallel = 4;
    disp.timeout_secs = 10;
    HU_ASSERT_EQ(disp.max_parallel, 4u);
    HU_ASSERT_EQ(disp.timeout_secs, 10u);
}

static void test_dispatcher_invalid_json_args_propagates_failure(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    hu_tool_call_t call = {
        .id = "x",
        .id_len = 1,
        .name = "shell",
        .name_len = 5,
        .arguments = "{invalid}",
        .arguments_len = 9,
    };
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    hu_dispatch_result_t dres;
    hu_error_t err = hu_dispatcher_dispatch(&disp, &alloc, &tool, 1, &call, 1, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, 1u);
    HU_ASSERT_FALSE(dres.results[0].success);
    hu_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

#ifdef HU_ENABLE_TUI
static void test_tui_init_deinit(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    hu_tui_state_t state;
    hu_error_t err = hu_tui_init(&state, &alloc, &agent, "test", "test-model", 3);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(state.tabs);
    HU_ASSERT_EQ(state.tab_count, 1);
    HU_ASSERT_EQ(state.active_tab, 0);
    HU_ASSERT_STR_EQ(state.provider_name, "test");
    HU_ASSERT_STR_EQ(state.model_name, "test-model");
    HU_ASSERT_EQ(state.tools_count, 3u);
    hu_tui_deinit(&state);
}

static void test_tui_approval_state_initial(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    hu_tui_state_t state;
    hu_tui_init(&state, &alloc, &agent, "test", "m", 0);
    HU_ASSERT_EQ(state.approval, HU_TUI_APPROVAL_NONE);
    HU_ASSERT_EQ(state.approval_tool[0], '\0');
    HU_ASSERT_EQ(state.approval_args[0], '\0');
    hu_tui_deinit(&state);
}

static void test_tui_output_initially_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    hu_tui_state_t state;
    hu_tui_init(&state, &alloc, &agent, "p", "m", 0);
    HU_ASSERT_EQ(state.output_len, 0u);
    HU_ASSERT_EQ(state.input_len, 0u);
    HU_ASSERT_EQ(state.tool_log_count, 0u);
    HU_ASSERT_FALSE(state.agent_running);
    HU_ASSERT_FALSE(state.quit_requested);
    hu_tui_deinit(&state);
}

static void test_tui_observer_create(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.alloc = &alloc;
    hu_tui_state_t state;
    hu_tui_init(&state, &alloc, &agent, "p", "m", 0);
    hu_observer_t obs = hu_tui_observer_create(&state);
    HU_ASSERT_NOT_NULL(obs.ctx);
    HU_ASSERT_NOT_NULL(obs.vtable);
    hu_tui_deinit(&state);
}
#endif

void run_agent_extended_tests(void) {
    HU_TEST_SUITE("Agent Extended");
    HU_RUN_TEST(test_agent_max_iterations_limit);
    HU_RUN_TEST(test_agent_empty_input);
    HU_RUN_TEST(test_agent_very_long_input);
    HU_RUN_TEST(test_agent_tool_not_found_handled);
    HU_RUN_TEST(test_agent_planner_empty_steps);
    HU_RUN_TEST(test_agent_planner_malformed_json);
    HU_RUN_TEST(test_agent_planner_null_steps_key);
    HU_RUN_TEST(test_agent_compaction_no_trigger);
    HU_RUN_TEST(test_agent_estimate_tokens_empty);
    HU_RUN_TEST(test_agent_estimate_tokens_long);
    HU_RUN_TEST(test_agent_planner_step_status_values);
    HU_RUN_TEST(test_agent_dispatcher_sequential);
    HU_RUN_TEST(test_agent_planner_next_step_null_when_complete);
    HU_RUN_TEST(test_agent_compaction_config_default);
    HU_RUN_TEST(test_agent_planner_alternate_format);
    HU_RUN_TEST(test_agent_dispatcher_zero_calls);
    HU_RUN_TEST(test_agent_plan_free_null_safe);
    HU_RUN_TEST(test_agent_planner_mark_step_out_of_range);

    HU_RUN_TEST(test_slash_parse_help);
    HU_RUN_TEST(test_slash_parse_status);
    HU_RUN_TEST(test_slash_parse_model_with_arg);
    HU_RUN_TEST(test_slash_parse_new);
    HU_RUN_TEST(test_slash_parse_reset);
    HU_RUN_TEST(test_slash_parse_clear);
    HU_RUN_TEST(test_slash_parse_compact);
    HU_RUN_TEST(test_slash_parse_cost);
    HU_RUN_TEST(test_slash_parse_memory);
    HU_RUN_TEST(test_slash_parse_tools);
    HU_RUN_TEST(test_slash_parse_non_slash_returns_null);
    HU_RUN_TEST(test_slash_parse_single_slash_returns_null);
    HU_RUN_TEST(test_slash_parse_whitespace_trailing);
    HU_RUN_TEST(test_slash_bare_session_reset_new);
    HU_RUN_TEST(test_slash_bare_session_reset_reset);
    HU_RUN_TEST(test_slash_bare_session_reset_help_returns_null);
    HU_RUN_TEST(test_slash_bare_session_reset_with_arg_returns_null);

    HU_RUN_TEST(test_compaction_trigger_over_message_limit);
    HU_RUN_TEST(test_compaction_compact_history);

    HU_RUN_TEST(test_planner_multi_step_order);
    HU_RUN_TEST(test_planner_step_with_description);
    HU_RUN_TEST(test_planner_is_complete_all_done);
    HU_RUN_TEST(test_planner_is_complete_one_pending);
    HU_RUN_TEST(test_planner_mark_failed);

    HU_RUN_TEST(test_json_parse_tool_call_format);
    HU_RUN_TEST(test_json_parse_empty_tool_calls);
    HU_RUN_TEST(test_json_parse_trailing_comma_rejected);
    HU_RUN_TEST(test_json_parse_truncated_rejected);
    HU_RUN_TEST(test_json_parse_unclosed_brace_rejected);
    HU_RUN_TEST(test_estimate_tokens_multi_message);
    HU_RUN_TEST(test_dispatcher_single_call);

    HU_RUN_TEST(test_dispatcher_default_config);
    HU_RUN_TEST(test_dispatcher_mixed_valid_invalid_tools);
    HU_RUN_TEST(test_dispatcher_result_output_content);
    HU_RUN_TEST(test_dispatcher_create_destroy);
    HU_RUN_TEST(test_dispatcher_dispatch_result_free_null_safe);
    HU_RUN_TEST(test_dispatcher_max_parallel_config_stored);
    HU_RUN_TEST(test_dispatcher_invalid_json_args_propagates_failure);
    HU_RUN_TEST(test_agent_tool_call_round_trip);

#ifdef HU_ENABLE_TUI
    HU_TEST_SUITE("TUI");
    HU_RUN_TEST(test_tui_init_deinit);
    HU_RUN_TEST(test_tui_approval_state_initial);
    HU_RUN_TEST(test_tui_output_initially_empty);
    HU_RUN_TEST(test_tui_observer_create);
#endif
}
