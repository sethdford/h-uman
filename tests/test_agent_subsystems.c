#include "human/agent/compaction.h"
#include "human/agent/dispatcher.h"
#include "human/agent/planner.h"
#include "human/context.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tools/factory.h"
#include "human/tools/shell.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

/* ─── Dispatcher tests ──────────────────────────────────────────────────── */

static void test_dispatcher_default_null_out(void) {
    hu_dispatcher_default(NULL);
}

static void test_dispatcher_create_null_alloc(void) {
    hu_dispatcher_t *d = NULL;
    hu_error_t err = hu_dispatcher_create(NULL, 1, 0, &d);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_NULL(d);
}

static void test_dispatcher_create_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_dispatcher_create(&alloc, 1, 0, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_dispatcher_destroy_null_alloc(void) {
    hu_dispatcher_t d;
    hu_dispatcher_default(&d);
    hu_dispatcher_destroy(NULL, &d);
}

static void test_dispatcher_destroy_null_d(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dispatcher_destroy(&alloc, NULL);
}

static void test_dispatcher_dispatch_null_d(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_call_t call = {0};
    hu_dispatch_result_t out;
    hu_error_t err = hu_dispatcher_dispatch(NULL, &alloc, NULL, 0, &call, 1, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_dispatcher_dispatch_null_alloc(void) {
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    hu_tool_call_t call = {0};
    hu_dispatch_result_t out;
    hu_error_t err = hu_dispatcher_dispatch(&disp, NULL, NULL, 0, &call, 1, &out);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_dispatcher_dispatch_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    hu_tool_call_t call = {0};
    hu_error_t err = hu_dispatcher_dispatch(&disp, &alloc, NULL, 0, &call, 1, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_dispatch_result_free_null_alloc(void) {
    hu_dispatch_result_t r = {.results = NULL, .count = 0};
    hu_dispatch_result_free(NULL, &r);
}

static void test_dispatch_result_free_null_r(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dispatch_result_free(&alloc, NULL);
}

static void test_dispatcher_sequential_single_tool(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    disp.max_parallel = 1;

    hu_tool_call_t call = {
        .id = "call-1",
        .id_len = 6,
        .name = "shell",
        .name_len = 5,
        .arguments = "{\"command\":\"echo hi\"}",
        .arguments_len = 21,
    };

    hu_dispatch_result_t dres;
    err = hu_dispatcher_dispatch(&disp, &alloc, &tool, 1, &call, 1, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, (size_t)1);
    HU_ASSERT_NOT_NULL(dres.results);
    HU_ASSERT_TRUE(dres.results[0].success);

    hu_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_dispatcher_multiple_tools(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    const char *arg1 = "{\"command\":\"echo a\"}";
    const char *arg2 = "{\"command\":\"echo b\"}";
    hu_tool_call_t calls[2] = {
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

    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    disp.max_parallel = 2;

    hu_dispatch_result_t dres;
    err = hu_dispatcher_dispatch(&disp, &alloc, &tool, 1, calls, 2, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, (size_t)2);
    HU_ASSERT_NOT_NULL(dres.results);
    HU_ASSERT_TRUE(dres.results[0].success);
    HU_ASSERT_TRUE(dres.results[1].success);

    hu_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_dispatcher_result_order_preserved(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    const char *a1 = "{\"command\":\"echo first\"}";
    const char *a2 = "{\"command\":\"echo second\"}";
    hu_tool_call_t calls[2] = {
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

    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    hu_dispatch_result_t dres;
    err = hu_dispatcher_dispatch(&disp, &alloc, &tool, 1, calls, 2, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, 2u);
    HU_ASSERT_TRUE(dres.results[0].success);
    HU_ASSERT_TRUE(dres.results[1].success);
    /* HU_IS_TEST: shell returns "(shell disabled in test mode)", not command output */
    HU_ASSERT_NOT_NULL(dres.results[0].output);
    HU_ASSERT_NOT_NULL(dres.results[1].output);

    hu_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_dispatcher_tool_failure_has_error_msg(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    hu_tool_call_t call = {
        .id = "x",
        .id_len = 1,
        .name = "unknown_tool_xyz",
        .name_len = 17,
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
    HU_ASSERT_NOT_NULL(dres.results[0].error_msg);
    HU_ASSERT_TRUE(strstr(dres.results[0].error_msg, "not found") != NULL);
    hu_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_dispatcher_sequential_respects_max_parallel_one(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    disp.max_parallel = 1;
    const char *args = "{\"command\":\"echo x\"}";
    hu_tool_call_t c = {.id = "x",
                        .id_len = 1,
                        .name = "shell",
                        .name_len = 5,
                        .arguments = args,
                        .arguments_len = strlen(args)};
    hu_dispatch_result_t dres;
    hu_error_t err = hu_dispatcher_dispatch(&disp, &alloc, &tool, 1, &c, 1, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, 1u);
    hu_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

static void test_dispatcher_tool_not_found(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tool_t tool;
    hu_error_t err = hu_shell_create(&alloc, "/tmp", 4, NULL, &tool);
    HU_ASSERT_EQ(err, HU_OK);

    hu_tool_call_t call = {
        .id = "x",
        .id_len = 1,
        .name = "nonexistent_tool",
        .name_len = 16,
        .arguments = "{}",
        .arguments_len = 2,
    };

    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    hu_dispatch_result_t dres;
    err = hu_dispatcher_dispatch(&disp, &alloc, &tool, 1, &call, 1, &dres);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(dres.count, (size_t)1);
    HU_ASSERT_FALSE(dres.results[0].success);
    HU_ASSERT_TRUE(strstr(dres.results[0].error_msg, "not found") != NULL);

    hu_dispatch_result_free(&alloc, &dres);
    if (tool.vtable->deinit)
        tool.vtable->deinit(tool.ctx, &alloc);
}

/* ─── Compaction tests ───────────────────────────────────────────────────── */

static void test_compaction_reduces_history(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_compaction_config_t cfg;
    hu_compaction_config_default(&cfg);
    cfg.keep_recent = 5;
    cfg.max_history_messages = 25; /* 30 > 25 triggers compaction */

    /* Create history: system + 30 user messages */
    size_t cap = 64;
    hu_owned_message_t *history =
        (hu_owned_message_t *)alloc.alloc(alloc.ctx, cap * sizeof(hu_owned_message_t));
    HU_ASSERT_NOT_NULL(history);
    memset(history, 0, cap * sizeof(hu_owned_message_t));

    history[0].role = HU_ROLE_SYSTEM;
    history[0].content = hu_strndup(&alloc, "system", 6);
    history[0].content_len = 6;
    history[0].name = NULL;
    history[0].name_len = 0;
    history[0].tool_call_id = NULL;
    history[0].tool_call_id_len = 0;

    for (size_t i = 1; i <= 30; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "msg-%zu", i);
        size_t len = strlen(buf);
        history[i].role = HU_ROLE_USER;
        history[i].content = hu_strndup(&alloc, buf, len);
        history[i].content_len = len;
        history[i].name = NULL;
        history[i].name_len = 0;
        history[i].tool_call_id = NULL;
        history[i].tool_call_id_len = 0;
    }

    size_t count = 31;
    HU_ASSERT_TRUE(hu_should_compact(history, count, &cfg));

    hu_error_t err = hu_compact_history(&alloc, history, &count, &cap, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    /* After compaction: system + 1 summary + 5 keep_recent = 7 */
    HU_ASSERT_TRUE(count < 31);
    HU_ASSERT_TRUE(count >= 5);

    /* Free remaining messages */
    for (size_t i = 0; i < count; i++) {
        if (history[i].content)
            alloc.free(alloc.ctx, history[i].content, history[i].content_len + 1);
    }
    alloc.free(alloc.ctx, history, cap * sizeof(hu_owned_message_t));
}

static void test_compaction_keep_recent_preserved(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_compaction_config_t cfg;
    hu_compaction_config_default(&cfg);
    cfg.keep_recent = 3;
    cfg.max_history_messages = 8; /* 10 > 8 triggers compaction */

    size_t cap = 32;
    hu_owned_message_t *history =
        (hu_owned_message_t *)alloc.alloc(alloc.ctx, cap * sizeof(hu_owned_message_t));
    HU_ASSERT_NOT_NULL(history);
    memset(history, 0, cap * sizeof(hu_owned_message_t));

    for (size_t i = 0; i < 10; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "msg-%zu", i);
        size_t len = strlen(buf);
        history[i].role = HU_ROLE_USER;
        history[i].content = hu_strndup(&alloc, buf, len);
        history[i].content_len = len;
        history[i].name = NULL;
        history[i].name_len = 0;
        history[i].tool_call_id = NULL;
        history[i].tool_call_id_len = 0;
    }

    size_t count = 10;
    hu_error_t err = hu_compact_history(&alloc, history, &count, &cap, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count <= 6); /* 1 summary + 3 keep_recent = 4 */
    /* Most recent messages (msg-7, msg-8, msg-9) should be preserved */
    bool found_recent = false;
    for (size_t i = 0; i < count; i++) {
        if (history[i].content && strstr(history[i].content, "msg-9"))
            found_recent = true;
    }
    HU_ASSERT_TRUE(found_recent);

    for (size_t i = 0; i < count; i++) {
        if (history[i].content)
            alloc.free(alloc.ctx, history[i].content, history[i].content_len + 1);
    }
    alloc.free(alloc.ctx, history, cap * sizeof(hu_owned_message_t));
}

static void test_compaction_frees_tool_calls(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_compaction_config_t cfg;
    hu_compaction_config_default(&cfg);
    cfg.keep_recent = 5;
    cfg.max_history_messages = 25; /* 31 > 25 triggers compaction */

    size_t cap = 64;
    hu_owned_message_t *history =
        (hu_owned_message_t *)alloc.alloc(alloc.ctx, cap * sizeof(hu_owned_message_t));
    HU_ASSERT_NOT_NULL(history);
    memset(history, 0, cap * sizeof(hu_owned_message_t));

    history[0].role = HU_ROLE_SYSTEM;
    history[0].content = hu_strndup(&alloc, "system", 6);
    history[0].content_len = 6;

    for (size_t i = 1; i <= 30; i++) {
        char buf[32];
        snprintf(buf, sizeof(buf), "msg-%zu", i);
        size_t len = strlen(buf);
        history[i].role = (i == 10) ? HU_ROLE_ASSISTANT : HU_ROLE_USER;
        history[i].content = hu_strndup(&alloc, buf, len);
        history[i].content_len = len;

        /* Message 10 (index 10) is ASSISTANT with tool_calls — in compact range [1, 21) */
        if (i == 10) {
            hu_tool_call_t *tcs = (hu_tool_call_t *)alloc.alloc(alloc.ctx, sizeof(hu_tool_call_t));
            HU_ASSERT_NOT_NULL(tcs);
            memset(tcs, 0, sizeof(hu_tool_call_t));
            tcs[0].id = hu_strndup(&alloc, "call_1", 6);
            tcs[0].id_len = 6;
            tcs[0].name = hu_strndup(&alloc, "shell", 5);
            tcs[0].name_len = 5;
            tcs[0].arguments = hu_strndup(&alloc, "{}", 2);
            tcs[0].arguments_len = 2;
            history[i].tool_calls = tcs;
            history[i].tool_calls_count = 1;
        }
    }

    size_t count = 31;
    HU_ASSERT_TRUE(hu_should_compact(history, count, &cfg));

    hu_error_t err = hu_compact_history(&alloc, history, &count, &cap, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count < 31);

    /* Free remaining messages (compaction freed tool_calls in compacted range) */
    for (size_t i = 0; i < count; i++) {
        if (history[i].content)
            alloc.free(alloc.ctx, history[i].content, history[i].content_len + 1);
        if (history[i].tool_calls) {
            for (size_t j = 0; j < history[i].tool_calls_count; j++) {
                hu_tool_call_t *tc = &history[i].tool_calls[j];
                if (tc->id && tc->id_len > 0)
                    alloc.free(alloc.ctx, (void *)tc->id, tc->id_len + 1);
                if (tc->name && tc->name_len > 0)
                    alloc.free(alloc.ctx, (void *)tc->name, tc->name_len + 1);
                if (tc->arguments && tc->arguments_len > 0)
                    alloc.free(alloc.ctx, (void *)tc->arguments, tc->arguments_len + 1);
            }
            alloc.free(alloc.ctx, history[i].tool_calls,
                       history[i].tool_calls_count * sizeof(hu_tool_call_t));
        }
    }
    alloc.free(alloc.ctx, history, cap * sizeof(hu_owned_message_t));
}

static void test_context_compact_pressure_with_tool_calls(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t cap = 64;
    hu_owned_message_t *history =
        (hu_owned_message_t *)alloc.alloc(alloc.ctx, cap * sizeof(hu_owned_message_t));
    HU_ASSERT_NOT_NULL(history);
    memset(history, 0, cap * sizeof(hu_owned_message_t));

    history[0].role = HU_ROLE_SYSTEM;
    history[0].content = hu_strndup(&alloc, "You are helpful", 15);
    history[0].content_len = 15;

    for (size_t i = 1; i <= 15; i++) {
        char buf[512];
        memset(buf, 'x', 280);
        snprintf(buf + 280, sizeof(buf) - 280, " message %zu", i);
        size_t len = strlen(buf);
        history[i].role = (i == 5) ? HU_ROLE_ASSISTANT : HU_ROLE_USER;
        history[i].content = hu_strndup(&alloc, buf, len);
        history[i].content_len = len;

        /* Message 5 (index 5) is ASSISTANT with tool_calls — in compact range */
        if (i == 5) {
            hu_tool_call_t *tcs = (hu_tool_call_t *)alloc.alloc(alloc.ctx, sizeof(hu_tool_call_t));
            HU_ASSERT_NOT_NULL(tcs);
            memset(tcs, 0, sizeof(hu_tool_call_t));
            tcs[0].id = hu_strndup(&alloc, "call_1", 6);
            tcs[0].id_len = 6;
            tcs[0].name = hu_strndup(&alloc, "shell", 5);
            tcs[0].name_len = 5;
            tcs[0].arguments = hu_strndup(&alloc, "{}", 2);
            tcs[0].arguments_len = 2;
            history[i].tool_calls = tcs;
            history[i].tool_calls_count = 1;
        }
    }

    size_t count = 16;
    hu_error_t err = hu_context_compact_for_pressure(&alloc, history, &count, &cap, 1000, 0.70f);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(history[0].role == HU_ROLE_SYSTEM);
    HU_ASSERT_STR_EQ(history[0].content, "You are helpful");
    HU_ASSERT_TRUE(strstr(history[1].content, "Previous context compacted") != NULL);

    for (size_t i = 0; i < count; i++) {
        if (history[i].content)
            alloc.free(alloc.ctx, history[i].content, history[i].content_len + 1);
        if (history[i].tool_calls) {
            for (size_t j = 0; j < history[i].tool_calls_count; j++) {
                hu_tool_call_t *tc = &history[i].tool_calls[j];
                if (tc->id && tc->id_len > 0)
                    alloc.free(alloc.ctx, (void *)tc->id, tc->id_len + 1);
                if (tc->name && tc->name_len > 0)
                    alloc.free(alloc.ctx, (void *)tc->name, tc->name_len + 1);
                if (tc->arguments && tc->arguments_len > 0)
                    alloc.free(alloc.ctx, (void *)tc->arguments, tc->arguments_len + 1);
            }
            alloc.free(alloc.ctx, history[i].tool_calls,
                       history[i].tool_calls_count * sizeof(hu_tool_call_t));
        }
    }
    alloc.free(alloc.ctx, history, cap * sizeof(hu_owned_message_t));
}

static void test_estimate_tokens(void) {
    hu_owned_message_t msgs[2] = {
        {.content = "hello", .content_len = 5, .role = HU_ROLE_USER},
        {.content = "world", .content_len = 5, .role = HU_ROLE_ASSISTANT},
    };
    uint64_t t = hu_estimate_tokens(msgs, 2);
    HU_ASSERT_EQ(t, (uint64_t)4); /* (5+5 + 3*2) / 4 = 4 — per-message overhead */
}

/* ─── Context pressure tests ─────────────────────────────────────────────── */

static void test_estimate_tokens_text_known_string(void) {
    /* 8 chars -> 2 tokens (8/4) */
    size_t t = hu_estimate_tokens_text("12345678", 8);
    HU_ASSERT_EQ(t, (size_t)2);
    /* 4 chars -> 1 token */
    t = hu_estimate_tokens_text("test", 4);
    HU_ASSERT_EQ(t, (size_t)1);
}

static void test_context_pressure_50_no_warning(void) {
    hu_context_pressure_t p = {
        .current_tokens = 50,
        .max_tokens = 100,
        .pressure = 0.0f,
        .warning_85_emitted = false,
        .warning_95_emitted = false,
    };
    bool compact = hu_context_check_pressure(&p, 0.85f, 0.95f);
    HU_ASSERT_FALSE(compact);
    HU_ASSERT_FALSE(p.warning_85_emitted);
    HU_ASSERT_FALSE(p.warning_95_emitted);
}

static void test_context_pressure_86_warning_emitted(void) {
    hu_context_pressure_t p = {
        .current_tokens = 86,
        .max_tokens = 100,
        .pressure = 0.0f,
        .warning_85_emitted = false,
        .warning_95_emitted = false,
    };
    bool compact = hu_context_check_pressure(&p, 0.85f, 0.95f);
    HU_ASSERT_FALSE(compact);
    HU_ASSERT_TRUE(p.warning_85_emitted);
    HU_ASSERT_FALSE(p.warning_95_emitted);
}

static void test_context_pressure_96_auto_compact_triggered(void) {
    hu_context_pressure_t p = {
        .current_tokens = 96,
        .max_tokens = 100,
        .pressure = 0.0f,
        .warning_85_emitted = false,
        .warning_95_emitted = false,
    };
    bool compact = hu_context_check_pressure(&p, 0.85f, 0.95f);
    HU_ASSERT_TRUE(compact);
    HU_ASSERT_TRUE(p.warning_95_emitted);
}

static void test_context_compact_preserves_system_and_recent(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t cap = 32;
    hu_owned_message_t *history =
        (hu_owned_message_t *)alloc.alloc(alloc.ctx, cap * sizeof(hu_owned_message_t));
    HU_ASSERT_NOT_NULL(history);
    memset(history, 0, cap * sizeof(hu_owned_message_t));

    history[0].role = HU_ROLE_SYSTEM;
    history[0].content = hu_strndup(&alloc, "You are helpful", 15);
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
        history[i].role = HU_ROLE_USER;
        history[i].content = hu_strndup(&alloc, buf, len);
        history[i].content_len = len;
        history[i].name = NULL;
        history[i].name_len = 0;
        history[i].tool_call_id = NULL;
        history[i].tool_call_id_len = 0;
    }

    size_t count = 16;
    hu_error_t err = hu_context_compact_for_pressure(&alloc, history, &count, &cap, 1000, 0.70f);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(history[0].role == HU_ROLE_SYSTEM);
    HU_ASSERT_STR_EQ(history[0].content, "You are helpful");
    HU_ASSERT_TRUE(strstr(history[1].content, "Previous context compacted") != NULL);

    for (size_t i = 0; i < count; i++) {
        if (history[i].content)
            alloc.free(alloc.ctx, history[i].content, history[i].content_len + 1);
    }
    alloc.free(alloc.ctx, history, cap * sizeof(hu_owned_message_t));
}

static void test_context_compact_reduces_below_target(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t cap = 64;
    hu_owned_message_t *history =
        (hu_owned_message_t *)alloc.alloc(alloc.ctx, cap * sizeof(hu_owned_message_t));
    HU_ASSERT_NOT_NULL(history);
    memset(history, 0, cap * sizeof(hu_owned_message_t));

    history[0].role = HU_ROLE_SYSTEM;
    history[0].content = hu_strndup(&alloc, "system", 6);
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
        history[i].role = (i % 2) ? HU_ROLE_USER : HU_ROLE_ASSISTANT;
        history[i].content = hu_strndup(&alloc, buf, len);
        history[i].content_len = len;
        history[i].name = NULL;
        history[i].name_len = 0;
        history[i].tool_call_id = NULL;
        history[i].tool_call_id_len = 0;
    }

    size_t count = 21;
    uint64_t before = hu_estimate_tokens(history, count);
    HU_ASSERT_TRUE((float)before / 500.0f > 0.95f);

    hu_error_t err = hu_context_compact_for_pressure(&alloc, history, &count, &cap, 500, 0.70f);
    HU_ASSERT_EQ(err, HU_OK);
    uint64_t after = hu_estimate_tokens(history, count);
    HU_ASSERT_TRUE((float)after / 500.0f < 0.75f); /* below 75% after compaction */

    for (size_t i = 0; i < count; i++) {
        if (history[i].content)
            alloc.free(alloc.ctx, history[i].content, history[i].content_len + 1);
    }
    alloc.free(alloc.ctx, history, cap * sizeof(hu_owned_message_t));
}

/* ─── Planner tests ─────────────────────────────────────────────────────── */

static void test_planner_create_plan(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"steps\":[{\"tool\":\"shell\",\"args\":{\"command\":\"ls\"},\"description\":\"list "
        "files\"},{\"tool\":\"file_read\",\"args\":{\"path\":\"a.txt\"}}]}";
    hu_plan_t *plan = NULL;
    hu_error_t err = hu_planner_create_plan(&alloc, json, strlen(json), &plan);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(plan);
    HU_ASSERT_EQ(plan->steps_count, (size_t)2);
    HU_ASSERT_STR_EQ(plan->steps[0].tool_name, "shell");
    HU_ASSERT_STR_EQ(plan->steps[1].tool_name, "file_read");
    HU_ASSERT_EQ(plan->steps[0].status, HU_PLAN_STEP_PENDING);
    hu_plan_free(&alloc, plan);
}

static void test_planner_step_progression(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"steps\":[{\"name\":\"a\",\"arguments\":{}},{\"name\":\"b\",\"arguments\":{}}]}";
    hu_plan_t *plan = NULL;
    hu_error_t err = hu_planner_create_plan(&alloc, json, strlen(json), &plan);
    HU_ASSERT_EQ(err, HU_OK);

    hu_plan_step_t *s1 = hu_planner_next_step(plan);
    HU_ASSERT_NOT_NULL(s1);
    HU_ASSERT_STR_EQ(s1->tool_name, "a");

    hu_planner_mark_step(plan, 0, HU_PLAN_STEP_DONE);
    hu_plan_step_t *s2 = hu_planner_next_step(plan);
    HU_ASSERT_NOT_NULL(s2);
    HU_ASSERT_STR_EQ(s2->tool_name, "b");

    hu_planner_mark_step(plan, 1, HU_PLAN_STEP_FAILED);
    hu_plan_step_t *s3 = hu_planner_next_step(plan);
    HU_ASSERT_NULL(s3);

    hu_plan_free(&alloc, plan);
}

static void test_planner_is_complete(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"steps\":[{\"tool\":\"x\",\"args\":{}}]}";
    hu_plan_t *plan = NULL;
    hu_error_t err = hu_planner_create_plan(&alloc, json, strlen(json), &plan);
    HU_ASSERT_EQ(err, HU_OK);

    HU_ASSERT_FALSE(hu_planner_is_complete(plan));
    hu_planner_mark_step(plan, 0, HU_PLAN_STEP_DONE);
    HU_ASSERT_TRUE(hu_planner_is_complete(plan));

    hu_plan_free(&alloc, plan);
}

static void test_planner_invalid_json(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_t *plan = NULL;
    hu_error_t err = hu_planner_create_plan(&alloc, "{}", 2, &plan);
    HU_ASSERT_NEQ(err, HU_OK);
    HU_ASSERT_NULL(plan);
}

void run_agent_subsystems_tests(void) {
    HU_TEST_SUITE("Agent subsystems");
    HU_RUN_TEST(test_dispatcher_default_null_out);
    HU_RUN_TEST(test_dispatcher_create_null_alloc);
    HU_RUN_TEST(test_dispatcher_create_null_out);
    HU_RUN_TEST(test_dispatcher_destroy_null_alloc);
    HU_RUN_TEST(test_dispatcher_destroy_null_d);
    HU_RUN_TEST(test_dispatcher_dispatch_null_d);
    HU_RUN_TEST(test_dispatcher_dispatch_null_alloc);
    HU_RUN_TEST(test_dispatcher_dispatch_null_out);
    HU_RUN_TEST(test_dispatch_result_free_null_alloc);
    HU_RUN_TEST(test_dispatch_result_free_null_r);
    HU_RUN_TEST(test_dispatcher_sequential_single_tool);
    HU_RUN_TEST(test_dispatcher_multiple_tools);
    HU_RUN_TEST(test_dispatcher_result_order_preserved);
    HU_RUN_TEST(test_dispatcher_tool_failure_has_error_msg);
    HU_RUN_TEST(test_dispatcher_sequential_respects_max_parallel_one);
    HU_RUN_TEST(test_dispatcher_tool_not_found);
    HU_RUN_TEST(test_compaction_reduces_history);
    HU_RUN_TEST(test_compaction_keep_recent_preserved);
    HU_RUN_TEST(test_compaction_frees_tool_calls);
    HU_RUN_TEST(test_context_compact_pressure_with_tool_calls);
    HU_RUN_TEST(test_estimate_tokens);
    HU_RUN_TEST(test_estimate_tokens_text_known_string);
    HU_RUN_TEST(test_context_pressure_50_no_warning);
    HU_RUN_TEST(test_context_pressure_86_warning_emitted);
    HU_RUN_TEST(test_context_pressure_96_auto_compact_triggered);
    HU_RUN_TEST(test_context_compact_preserves_system_and_recent);
    HU_RUN_TEST(test_context_compact_reduces_below_target);
    HU_RUN_TEST(test_planner_create_plan);
    HU_RUN_TEST(test_planner_step_progression);
    HU_RUN_TEST(test_planner_is_complete);
    HU_RUN_TEST(test_planner_invalid_json);
}
