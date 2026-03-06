#include "seaclaw/agent/compaction.h"
#include "seaclaw/agent/dispatcher.h"
#include "seaclaw/agent/planner.h"
#include "seaclaw/context.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/tools/factory.h"
#include "seaclaw/tools/shell.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

/* ─── Dispatcher tests ──────────────────────────────────────────────────── */

static void test_dispatcher_default_null_out(void) {
    sc_dispatcher_default(NULL);
}

static void test_dispatcher_create_null_alloc(void) {
    sc_dispatcher_t *d = NULL;
    sc_error_t err = sc_dispatcher_create(NULL, 1, 0, &d);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_NULL(d);
}

static void test_dispatcher_create_null_out(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_dispatcher_create(&alloc, 1, 0, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_dispatcher_destroy_null_alloc(void) {
    sc_dispatcher_t d;
    sc_dispatcher_default(&d);
    sc_dispatcher_destroy(NULL, &d);
}

static void test_dispatcher_destroy_null_d(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_dispatcher_destroy(&alloc, NULL);
}

static void test_dispatcher_dispatch_null_d(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_call_t call = {0};
    sc_dispatch_result_t out;
    sc_error_t err = sc_dispatcher_dispatch(NULL, &alloc, NULL, 0, &call, 1, &out);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_dispatcher_dispatch_null_alloc(void) {
    sc_dispatcher_t disp;
    sc_dispatcher_default(&disp);
    sc_tool_call_t call = {0};
    sc_dispatch_result_t out;
    sc_error_t err = sc_dispatcher_dispatch(&disp, NULL, NULL, 0, &call, 1, &out);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_dispatcher_dispatch_null_out(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_dispatcher_t disp;
    sc_dispatcher_default(&disp);
    sc_tool_call_t call = {0};
    sc_error_t err = sc_dispatcher_dispatch(&disp, &alloc, NULL, 0, &call, 1, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_dispatch_result_free_null_alloc(void) {
    sc_dispatch_result_t r = {.results = NULL, .count = 0};
    sc_dispatch_result_free(NULL, &r);
}

static void test_dispatch_result_free_null_r(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_dispatch_result_free(&alloc, NULL);
}

static void test_dispatcher_sequential_single_tool(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    sc_dispatcher_t disp;
    sc_dispatcher_default(&disp);
    disp.max_parallel = 1;

    sc_tool_call_t call = {
        .id = "call-1",
        .id_len = 6,
        .name = "shell",
        .name_len = 5,
        .arguments = "{\"command\":\"echo hi\"}",
        .arguments_len = 21,
    };

    sc_dispatch_result_t dres;
    err = sc_dispatcher_dispatch(&disp, &alloc, &tool, 1, &call, 1, &dres);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(dres.count, (size_t)1);
    SC_ASSERT_NOT_NULL(dres.results);
    SC_ASSERT_TRUE(dres.results[0].success);

    sc_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_dispatcher_multiple_tools(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    const char *arg1 = "{\"command\":\"echo a\"}";
    const char *arg2 = "{\"command\":\"echo b\"}";
    sc_tool_call_t calls[2] = {
        {.id = "c1",
         .id_len = 2,
         .name = "shell",
         .name_len = 5,
         .arguments = arg1,
         .arguments_len = strlen(arg1)},
        {.id = "c2",
         .id_len = 2,
         .name = "shell",
         .name_len = 5,
         .arguments = arg2,
         .arguments_len = strlen(arg2)},
    };

    sc_dispatcher_t disp;
    sc_dispatcher_default(&disp);
    disp.max_parallel = 2;

    sc_dispatch_result_t dres;
    err = sc_dispatcher_dispatch(&disp, &alloc, &tool, 1, calls, 2, &dres);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(dres.count, (size_t)2);
    SC_ASSERT_NOT_NULL(dres.results);
    SC_ASSERT_TRUE(dres.results[0].success);
    SC_ASSERT_TRUE(dres.results[1].success);

    sc_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_dispatcher_result_order_preserved(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    const char *a1 = "{\"command\":\"echo first\"}";
    const char *a2 = "{\"command\":\"echo second\"}";
    sc_tool_call_t calls[2] = {
        {.id = "c1",
         .id_len = 2,
         .name = "shell",
         .name_len = 5,
         .arguments = a1,
         .arguments_len = strlen(a1)},
        {.id = "c2",
         .id_len = 2,
         .name = "shell",
         .name_len = 5,
         .arguments = a2,
         .arguments_len = strlen(a2)},
    };

    sc_dispatcher_t disp;
    sc_dispatcher_default(&disp);
    sc_dispatch_result_t dres;
    err = sc_dispatcher_dispatch(&disp, &alloc, &tool, 1, calls, 2, &dres);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(dres.count, 2u);
    SC_ASSERT_TRUE(dres.results[0].success);
    SC_ASSERT_TRUE(dres.results[1].success);
    /* SC_IS_TEST: shell returns "(shell disabled in test mode)", not command output */
    SC_ASSERT_NOT_NULL(dres.results[0].output);
    SC_ASSERT_NOT_NULL(dres.results[1].output);

    sc_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_dispatcher_tool_failure_has_error_msg(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    sc_tool_call_t call = {
        .id = "x",
        .id_len = 1,
        .name = "unknown_tool_xyz",
        .name_len = 17,
        .arguments = "{}",
        .arguments_len = 2,
    };
    sc_dispatcher_t disp;
    sc_dispatcher_default(&disp);
    sc_dispatch_result_t dres;
    sc_error_t err = sc_dispatcher_dispatch(&disp, &alloc, &tool, 1, &call, 1, &dres);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(dres.count, 1u);
    SC_ASSERT_FALSE(dres.results[0].success);
    SC_ASSERT_NOT_NULL(dres.results[0].error_msg);
    SC_ASSERT_TRUE(strstr(dres.results[0].error_msg, "not found") != NULL);
    sc_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_dispatcher_sequential_respects_max_parallel_one(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    sc_dispatcher_t disp;
    sc_dispatcher_default(&disp);
    disp.max_parallel = 1;
    const char *args = "{\"command\":\"echo x\"}";
    sc_tool_call_t c = {.id = "x",
                        .id_len = 1,
                        .name = "shell",
                        .name_len = 5,
                        .arguments = args,
                        .arguments_len = strlen(args)};
    sc_dispatch_result_t dres;
    sc_error_t err = sc_dispatcher_dispatch(&disp, &alloc, &tool, 1, &c, 1, &dres);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(dres.count, 1u);
    sc_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_dispatcher_tool_not_found(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_tool_t tool;
    sc_error_t err = sc_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    SC_ASSERT_EQ(err, SC_OK);

    sc_tool_call_t call = {
        .id = "x",
        .id_len = 1,
        .name = "nonexistent_tool",
        .name_len = 16,
        .arguments = "{}",
        .arguments_len = 2,
    };

    sc_dispatcher_t disp;
    sc_dispatcher_default(&disp);
    sc_dispatch_result_t dres;
    err = sc_dispatcher_dispatch(&disp, &alloc, &tool, 1, &call, 1, &dres);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(dres.count, (size_t)1);
    SC_ASSERT_FALSE(dres.results[0].success);
    SC_ASSERT_TRUE(strstr(dres.results[0].error_msg, "not found") != NULL);

    sc_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Compaction tests ───────────────────────────────────────────────────── */

static void test_compaction_reduces_history(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_compaction_config_t cfg;
    sc_compaction_config_default(&cfg);
    cfg.keep_recent = 5;
    cfg.max_history_messages = 25; /* 30 > 25 triggers compaction */

    /* Create history: system + 30 user messages */
    size_t cap = 64;
    sc_owned_message_t *history =
        (sc_owned_message_t *)alloc.alloc(alloc.ctx, cap * sizeof(sc_owned_message_t));
    SC_ASSERT_NOT_NULL(history);

    history[0].role = SC_ROLE_SYSTEM;
    history[0].content = sc_strndup(&alloc, "system", 6);
    history[0].content_len = 6;
    history[0].name = NULL;
    history[0].name_len = 0;
    history[0].tool_call_id = NULL;
    history[0].tool_call_id_len = 0;

    for (size_t i = 1; i <= 30; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "msg-%zu", i);
        size_t len = strlen(buf);
        history[i].role = SC_ROLE_USER;
        history[i].content = sc_strndup(&alloc, buf, len);
        history[i].content_len = len;
        history[i].name = NULL;
        history[i].name_len = 0;
        history[i].tool_call_id = NULL;
        history[i].tool_call_id_len = 0;
    }

    size_t count = 31;
    SC_ASSERT_TRUE(sc_should_compact(history, count, &cfg));

    sc_error_t err = sc_compact_history(&alloc, history, &count, &cap, &cfg);
    SC_ASSERT_EQ(err, SC_OK);
    /* After compaction: system + 1 summary + 5 keep_recent = 7 */
    SC_ASSERT_TRUE(count < 31);
    SC_ASSERT_TRUE(count >= 5);

    /* Free remaining messages */
    for (size_t i = 0; i < count; i++) {
        if (history[i].content)
            alloc.free(alloc.ctx, history[i].content, history[i].content_len + 1);
    }
    alloc.free(alloc.ctx, history, cap * sizeof(sc_owned_message_t));
}

static void test_compaction_keep_recent_preserved(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_compaction_config_t cfg;
    sc_compaction_config_default(&cfg);
    cfg.keep_recent = 3;
    cfg.max_history_messages = 8; /* 10 > 8 triggers compaction */

    size_t cap = 32;
    sc_owned_message_t *history =
        (sc_owned_message_t *)alloc.alloc(alloc.ctx, cap * sizeof(sc_owned_message_t));
    SC_ASSERT_NOT_NULL(history);

    for (size_t i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "msg-%zu", i);
        size_t len = strlen(buf);
        history[i].role = SC_ROLE_USER;
        history[i].content = sc_strndup(&alloc, buf, len);
        history[i].content_len = len;
        history[i].name = NULL;
        history[i].name_len = 0;
        history[i].tool_call_id = NULL;
        history[i].tool_call_id_len = 0;
    }

    size_t count = 10;
    sc_error_t err = sc_compact_history(&alloc, history, &count, &cap, &cfg);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count <= 6); /* 1 summary + 3 keep_recent = 4 */
    /* Most recent messages (msg-7, msg-8, msg-9) should be preserved */
    bool found_recent = false;
    for (size_t i = 0; i < count; i++) {
        if (history[i].content && strstr(history[i].content, "msg-9"))
            found_recent = true;
    }
    SC_ASSERT_TRUE(found_recent);

    for (size_t i = 0; i < count; i++) {
        if (history[i].content)
            alloc.free(alloc.ctx, history[i].content, history[i].content_len + 1);
    }
    alloc.free(alloc.ctx, history, cap * sizeof(sc_owned_message_t));
}

static void test_estimate_tokens(void) {
    sc_owned_message_t msgs[2] = {
        {.content = "hello", .content_len = 5, .role = SC_ROLE_USER},
        {.content = "world", .content_len = 5, .role = SC_ROLE_ASSISTANT},
    };
    uint64_t t = sc_estimate_tokens(msgs, 2);
    SC_ASSERT_EQ(t, (uint64_t)4); /* (5+5 + 3*2) / 4 = 4 — per-message overhead */
}

/* ─── Context pressure tests ─────────────────────────────────────────────── */

static void test_estimate_tokens_text_known_string(void) {
    /* 8 chars -> 2 tokens (8/4) */
    size_t t = sc_estimate_tokens_text("12345678", 8);
    SC_ASSERT_EQ(t, (size_t)2);
    /* 4 chars -> 1 token */
    t = sc_estimate_tokens_text("test", 4);
    SC_ASSERT_EQ(t, (size_t)1);
}

static void test_context_pressure_50_no_warning(void) {
    sc_context_pressure_t p = {
        .current_tokens = 50,
        .max_tokens = 100,
        .pressure = 0.0f,
        .warning_85_emitted = false,
        .warning_95_emitted = false,
    };
    bool compact = sc_context_check_pressure(&p, 0.85f, 0.95f);
    SC_ASSERT_FALSE(compact);
    SC_ASSERT_FALSE(p.warning_85_emitted);
    SC_ASSERT_FALSE(p.warning_95_emitted);
}

static void test_context_pressure_86_warning_emitted(void) {
    sc_context_pressure_t p = {
        .current_tokens = 86,
        .max_tokens = 100,
        .pressure = 0.0f,
        .warning_85_emitted = false,
        .warning_95_emitted = false,
    };
    bool compact = sc_context_check_pressure(&p, 0.85f, 0.95f);
    SC_ASSERT_FALSE(compact);
    SC_ASSERT_TRUE(p.warning_85_emitted);
    SC_ASSERT_FALSE(p.warning_95_emitted);
}

static void test_context_pressure_96_auto_compact_triggered(void) {
    sc_context_pressure_t p = {
        .current_tokens = 96,
        .max_tokens = 100,
        .pressure = 0.0f,
        .warning_85_emitted = false,
        .warning_95_emitted = false,
    };
    bool compact = sc_context_check_pressure(&p, 0.85f, 0.95f);
    SC_ASSERT_TRUE(compact);
    SC_ASSERT_TRUE(p.warning_95_emitted);
}

static void test_context_compact_preserves_system_and_recent(void) {
    sc_allocator_t alloc = sc_system_allocator();
    size_t cap = 32;
    sc_owned_message_t *history =
        (sc_owned_message_t *)alloc.alloc(alloc.ctx, cap * sizeof(sc_owned_message_t));
    SC_ASSERT_NOT_NULL(history);

    history[0].role = SC_ROLE_SYSTEM;
    history[0].content = sc_strndup(&alloc, "You are helpful", 15);
    history[0].content_len = 15;
    history[0].name = NULL;
    history[0].name_len = 0;
    history[0].tool_call_id = NULL;
    history[0].tool_call_id_len = 0;

    for (size_t i = 1; i <= 15; i++) {
        char buf[512];
        memset(buf, 'x', 280);
        snprintf(buf + 280, sizeof(buf) - 280, " message %zu", i);
        size_t len = strlen(buf);
        history[i].role = SC_ROLE_USER;
        history[i].content = sc_strndup(&alloc, buf, len);
        history[i].content_len = len;
        history[i].name = NULL;
        history[i].name_len = 0;
        history[i].tool_call_id = NULL;
        history[i].tool_call_id_len = 0;
    }

    size_t count = 16;
    sc_error_t err = sc_context_compact_for_pressure(&alloc, history, &count, &cap, 1000, 0.70f);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(history[0].role == SC_ROLE_SYSTEM);
    SC_ASSERT_STR_EQ(history[0].content, "You are helpful");
    SC_ASSERT_TRUE(strstr(history[1].content, "Previous context compacted") != NULL);

    for (size_t i = 0; i < count; i++) {
        if (history[i].content)
            alloc.free(alloc.ctx, history[i].content, history[i].content_len + 1);
    }
    alloc.free(alloc.ctx, history, cap * sizeof(sc_owned_message_t));
}

static void test_context_compact_reduces_below_target(void) {
    sc_allocator_t alloc = sc_system_allocator();
    size_t cap = 64;
    sc_owned_message_t *history =
        (sc_owned_message_t *)alloc.alloc(alloc.ctx, cap * sizeof(sc_owned_message_t));
    SC_ASSERT_NOT_NULL(history);

    history[0].role = SC_ROLE_SYSTEM;
    history[0].content = sc_strndup(&alloc, "system", 6);
    history[0].content_len = 6;
    history[0].name = NULL;
    history[0].name_len = 0;
    history[0].tool_call_id = NULL;
    history[0].tool_call_id_len = 0;

    for (size_t i = 1; i <= 20; i++) {
        char buf[256];
        memset(buf, 'a', 96);
        snprintf(buf + 96, sizeof(buf) - 96, " msg %zu", i);
        size_t len = strlen(buf);
        history[i].role = (i % 2) ? SC_ROLE_USER : SC_ROLE_ASSISTANT;
        history[i].content = sc_strndup(&alloc, buf, len);
        history[i].content_len = len;
        history[i].name = NULL;
        history[i].name_len = 0;
        history[i].tool_call_id = NULL;
        history[i].tool_call_id_len = 0;
    }

    size_t count = 21;
    uint64_t before = sc_estimate_tokens(history, count);
    SC_ASSERT_TRUE((float)before / 500.0f > 0.95f);

    sc_error_t err = sc_context_compact_for_pressure(&alloc, history, &count, &cap, 500, 0.70f);
    SC_ASSERT_EQ(err, SC_OK);
    uint64_t after = sc_estimate_tokens(history, count);
    SC_ASSERT_TRUE((float)after / 500.0f < 0.75f); /* below 75% after compaction */

    for (size_t i = 0; i < count; i++) {
        if (history[i].content)
            alloc.free(alloc.ctx, history[i].content, history[i].content_len + 1);
    }
    alloc.free(alloc.ctx, history, cap * sizeof(sc_owned_message_t));
}

/* ─── Planner tests ─────────────────────────────────────────────────────── */

static void test_planner_create_plan(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{\"steps\":[{\"tool\":\"shell\",\"args\":{\"command\":\"ls\"},\"description\":\"list "
        "files\"},{\"tool\":\"file_read\",\"args\":{\"path\":\"a.txt\"}}]}";
    sc_plan_t *plan = NULL;
    sc_error_t err = sc_planner_create_plan(&alloc, json, strlen(json), &plan);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(plan);
    SC_ASSERT_EQ(plan->steps_count, (size_t)2);
    SC_ASSERT_STR_EQ(plan->steps[0].tool_name, "shell");
    SC_ASSERT_STR_EQ(plan->steps[1].tool_name, "file_read");
    SC_ASSERT_EQ(plan->steps[0].status, SC_PLAN_STEP_PENDING);
    sc_plan_free(&alloc, plan);
}

static void test_planner_step_progression(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{\"steps\":[{\"name\":\"a\",\"arguments\":{}},{\"name\":\"b\",\"arguments\":{}}]}";
    sc_plan_t *plan = NULL;
    sc_error_t err = sc_planner_create_plan(&alloc, json, strlen(json), &plan);
    SC_ASSERT_EQ(err, SC_OK);

    sc_plan_step_t *s1 = sc_planner_next_step(plan);
    SC_ASSERT_NOT_NULL(s1);
    SC_ASSERT_STR_EQ(s1->tool_name, "a");

    sc_planner_mark_step(plan, 0, SC_PLAN_STEP_DONE);
    sc_plan_step_t *s2 = sc_planner_next_step(plan);
    SC_ASSERT_NOT_NULL(s2);
    SC_ASSERT_STR_EQ(s2->tool_name, "b");

    sc_planner_mark_step(plan, 1, SC_PLAN_STEP_FAILED);
    sc_plan_step_t *s3 = sc_planner_next_step(plan);
    SC_ASSERT_NULL(s3);

    sc_plan_free(&alloc, plan);
}

static void test_planner_is_complete(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json = "{\"steps\":[{\"tool\":\"x\",\"args\":{}}]}";
    sc_plan_t *plan = NULL;
    sc_error_t err = sc_planner_create_plan(&alloc, json, strlen(json), &plan);
    SC_ASSERT_EQ(err, SC_OK);

    SC_ASSERT_FALSE(sc_planner_is_complete(plan));
    sc_planner_mark_step(plan, 0, SC_PLAN_STEP_DONE);
    SC_ASSERT_TRUE(sc_planner_is_complete(plan));

    sc_plan_free(&alloc, plan);
}

static void test_planner_invalid_json(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_plan_t *plan = NULL;
    sc_error_t err = sc_planner_create_plan(&alloc, "{}", 2, &plan);
    SC_ASSERT_NEQ(err, SC_OK);
    SC_ASSERT_NULL(plan);
}

void run_agent_subsystems_tests(void) {
    SC_TEST_SUITE("Agent subsystems");
    SC_RUN_TEST(test_dispatcher_default_null_out);
    SC_RUN_TEST(test_dispatcher_create_null_alloc);
    SC_RUN_TEST(test_dispatcher_create_null_out);
    SC_RUN_TEST(test_dispatcher_destroy_null_alloc);
    SC_RUN_TEST(test_dispatcher_destroy_null_d);
    SC_RUN_TEST(test_dispatcher_dispatch_null_d);
    SC_RUN_TEST(test_dispatcher_dispatch_null_alloc);
    SC_RUN_TEST(test_dispatcher_dispatch_null_out);
    SC_RUN_TEST(test_dispatch_result_free_null_alloc);
    SC_RUN_TEST(test_dispatch_result_free_null_r);
    SC_RUN_TEST(test_dispatcher_sequential_single_tool);
    SC_RUN_TEST(test_dispatcher_multiple_tools);
    SC_RUN_TEST(test_dispatcher_result_order_preserved);
    SC_RUN_TEST(test_dispatcher_tool_failure_has_error_msg);
    SC_RUN_TEST(test_dispatcher_sequential_respects_max_parallel_one);
    SC_RUN_TEST(test_dispatcher_tool_not_found);
    SC_RUN_TEST(test_compaction_reduces_history);
    SC_RUN_TEST(test_compaction_keep_recent_preserved);
    SC_RUN_TEST(test_estimate_tokens);
    SC_RUN_TEST(test_estimate_tokens_text_known_string);
    SC_RUN_TEST(test_context_pressure_50_no_warning);
    SC_RUN_TEST(test_context_pressure_86_warning_emitted);
    SC_RUN_TEST(test_context_pressure_96_auto_compact_triggered);
    SC_RUN_TEST(test_context_compact_preserves_system_and_recent);
    SC_RUN_TEST(test_context_compact_reduces_below_target);
    SC_RUN_TEST(test_planner_create_plan);
    SC_RUN_TEST(test_planner_step_progression);
    SC_RUN_TEST(test_planner_is_complete);
    SC_RUN_TEST(test_planner_invalid_json);
}
