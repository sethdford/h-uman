/* Tests for thin agent modules: outcomes, awareness, action_preview, episodic,
 * memory_loader, context_tokens, dispatcher, mailbox, compaction, reflection, planner. */
#include "seaclaw/agent/action_preview.h"
#include "seaclaw/agent/awareness.h"
#include "seaclaw/agent/compaction.h"
#include "seaclaw/agent/dispatcher.h"
#include "seaclaw/agent/episodic.h"
#include "seaclaw/agent/mailbox.h"
#include "seaclaw/agent/memory_loader.h"
#include "seaclaw/agent/outcomes.h"
#include "seaclaw/agent/planner.h"
#include "seaclaw/agent/reflection.h"
#include "seaclaw/bus.h"
#include "seaclaw/config_types.h"
#include "seaclaw/context_tokens.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/engines.h"
#include "test_framework.h"
#include <string.h>

/* ─── outcomes ───────────────────────────────────────────────────────────── */

static void test_outcomes_record_and_retrieve(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);

    sc_outcome_record_tool(&tracker, "file_read", true, "read config");
    sc_outcome_record_tool(&tracker, "shell", false, "command failed");

    size_t count = 0;
    const sc_outcome_entry_t *entries = sc_outcome_get_recent(&tracker, &count);
    SC_ASSERT_NOT_NULL(entries);
    SC_ASSERT_EQ(count, 2u);
    SC_ASSERT_EQ(tracker.tool_successes, 1u);
    SC_ASSERT_EQ(tracker.tool_failures, 1u);

    char *summary = sc_outcome_build_summary(&tracker, &alloc, NULL);
    SC_ASSERT_NOT_NULL(summary);
    SC_ASSERT_TRUE(strstr(summary, "succeeded") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "failed") != NULL);
    alloc.free(alloc.ctx, summary, strlen(summary) + 1);
}

static void test_outcomes_detect_repeated_failure(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);

    sc_outcome_record_tool(&tracker, "shell", false, "fail1");
    sc_outcome_record_tool(&tracker, "shell", false, "fail2");
    sc_outcome_record_tool(&tracker, "shell", false, "fail3");

    SC_ASSERT_TRUE(sc_outcome_detect_repeated_failure(&tracker, "shell", 3));
    SC_ASSERT_FALSE(sc_outcome_detect_repeated_failure(&tracker, "shell", 4));
    SC_ASSERT_FALSE(sc_outcome_detect_repeated_failure(&tracker, "file_read", 1));
}

/* ─── awareness ─────────────────────────────────────────────────────────── */

static void test_awareness_init_with_bus(void) {
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_awareness_t aw;
    sc_error_t err = sc_awareness_init(&aw, &bus);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(aw.state.messages_received, 0u);
    sc_awareness_deinit(&aw);
}

static void test_awareness_context_returns_non_null_when_data(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_awareness_t aw;
    sc_awareness_init(&aw, &bus);

    sc_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SC_BUS_MESSAGE_RECEIVED;
    strncpy(ev.channel, "cli", SC_BUS_CHANNEL_LEN);
    sc_bus_publish(&bus, &ev);

    size_t len = 0;
    char *ctx = sc_awareness_context(&aw, &alloc, &len);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(strstr(ctx, "Situational Awareness") != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);
    sc_awareness_deinit(&aw);
}

static void test_awareness_context_null_when_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_awareness_t aw;
    sc_awareness_init(&aw, &bus);

    size_t len = 0;
    char *ctx = sc_awareness_context(&aw, &alloc, &len);
    SC_ASSERT_NULL(ctx);
    sc_awareness_deinit(&aw);
}

/* ─── action_preview ────────────────────────────────────────────────────── */

static void test_action_preview_generate_simple(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_action_preview_t p;
    const char *args = "{\"path\":\"/tmp/test.txt\"}";
    sc_error_t err = sc_action_preview_generate(&alloc, "file_read", args, strlen(args), &p);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(p.description);
    SC_ASSERT_TRUE(strstr(p.description, "/tmp/test.txt") != NULL);
    SC_ASSERT_STR_EQ(p.risk_level, "low");

    char *formatted = NULL;
    size_t fmt_len = 0;
    err = sc_action_preview_format(&alloc, &p, &formatted, &fmt_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(formatted);
    SC_ASSERT_TRUE(strstr(formatted, "file_read") != NULL);
    alloc.free(alloc.ctx, formatted, fmt_len + 1);
    sc_action_preview_free(&alloc, &p);
}

/* ─── episodic ───────────────────────────────────────────────────────────── */

static void test_episodic_summarize_and_store_recall(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *msgs[] = {"What is the config format?", "Here is the config..."};
    size_t lens[] = {strlen(msgs[0]), strlen(msgs[1])};
    size_t out_len = 0;

    char *summary = sc_episodic_summarize_session(&alloc, msgs, lens, 2, &out_len);
    SC_ASSERT_NOT_NULL(summary);
    SC_ASSERT_TRUE(out_len > 0);
    SC_ASSERT_TRUE(strstr(summary, "config format") != NULL);

    sc_memory_t mem = sc_memory_lru_create(&alloc, 10);
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_error_t err = sc_episodic_store(&mem, &alloc, "sess_1", 6, summary, out_len);
    SC_ASSERT_EQ(err, SC_OK);
    alloc.free(alloc.ctx, summary, out_len + 1);

    char *loaded = NULL;
    size_t loaded_len = 0;
    err = sc_episodic_load(&mem, &alloc, &loaded, &loaded_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(loaded);
    SC_ASSERT_TRUE(loaded_len > 0);
    SC_ASSERT_TRUE(strstr(loaded, "config format") != NULL);
    alloc.free(alloc.ctx, loaded, loaded_len + 1);
    mem.vtable->deinit(mem.ctx);
}

/* ─── memory_loader ──────────────────────────────────────────────────────── */

static void test_memory_loader_null_memory_graceful(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_loader_t loader;
    sc_error_t err = sc_memory_loader_init(&loader, &alloc, NULL, NULL, 10, 4000);
    SC_ASSERT_EQ(err, SC_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    err = sc_memory_loader_load(&loader, "query", 5, NULL, 0, &ctx, &ctx_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NULL(ctx);
    SC_ASSERT_EQ(ctx_len, 0u);
}

/* ─── context_tokens ────────────────────────────────────────────────────── */

static void test_context_tokens_lookup_known_model(void) {
    uint64_t v = sc_context_tokens_lookup("claude-sonnet-4.6", 17);
    SC_ASSERT_EQ(v, 200000u);

    v = sc_context_tokens_lookup("gpt-4.1", 7);
    SC_ASSERT_EQ(v, 128000u);
}

static void test_context_tokens_default(void) {
    uint64_t v = sc_context_tokens_default();
    SC_ASSERT_EQ(v, SC_DEFAULT_AGENT_TOKEN_LIMIT);
}

static void test_context_tokens_resolve(void) {
    uint64_t v = sc_context_tokens_resolve(50000, "unknown", 7);
    SC_ASSERT_EQ(v, 50000u);

    v = sc_context_tokens_resolve(0, "claude-opus-4-6", 14);
    SC_ASSERT_EQ(v, 200000u);
}

/* ─── dispatcher ─────────────────────────────────────────────────────────── */

static void test_dispatcher_exists_handles_basic_input(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_dispatcher_t disp;
    sc_dispatcher_default(&disp);
    SC_ASSERT_EQ(disp.max_parallel, 1u);

    sc_tool_call_t calls[1] = {{.id = "x", .id_len = 1, .name = "nonexistent", .name_len = 10,
                                .arguments = "{}", .arguments_len = 2}};
    sc_dispatch_result_t out;
    sc_error_t err = sc_dispatcher_dispatch(&disp, &alloc, NULL, 0, calls, 1, &out);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(out.count, 1u);
    SC_ASSERT_NOT_NULL(out.results);
    SC_ASSERT_FALSE(out.results[0].success);
    sc_dispatch_result_free(&alloc, &out);
}

/* ─── mailbox ────────────────────────────────────────────────────────────── */

static void test_mailbox_send_recv_roundtrip(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_mailbox_t *mbox = sc_mailbox_create(&alloc, 4);
    SC_ASSERT_NOT_NULL(mbox);

    sc_error_t err = sc_mailbox_register(mbox, 1);
    SC_ASSERT_EQ(err, SC_OK);
    err = sc_mailbox_register(mbox, 2);
    SC_ASSERT_EQ(err, SC_OK);

    const char *payload = "hello";
    err = sc_mailbox_send(mbox, 1, 2, SC_MSG_TASK, payload, strlen(payload), 0);
    SC_ASSERT_EQ(err, SC_OK);

    sc_message_t msg;
    err = sc_mailbox_recv(mbox, 2, &msg);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(msg.type, SC_MSG_TASK);
    SC_ASSERT_EQ(msg.from_agent, 1u);
    SC_ASSERT_EQ(msg.to_agent, 2u);
    SC_ASSERT_NOT_NULL(msg.payload);
    SC_ASSERT_STR_EQ(msg.payload, "hello");
    sc_message_free(&alloc, &msg);

    SC_ASSERT_EQ(sc_mailbox_pending_count(mbox, 2), 0u);
    sc_mailbox_destroy(mbox);
}

/* ─── compaction ─────────────────────────────────────────────────────────── */

static void test_compaction_message_compaction(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_compaction_config_t cfg;
    sc_compaction_config_default(&cfg);
    cfg.keep_recent = 2;
    cfg.max_history_messages = 5;

    sc_owned_message_t msgs[6];
    for (int i = 0; i < 6; i++) {
        msgs[i].role = i == 0 ? SC_ROLE_SYSTEM : SC_ROLE_USER;
        msgs[i].content = alloc.alloc(alloc.ctx, 2);
        SC_ASSERT_NOT_NULL(msgs[i].content);
        msgs[i].content[0] = 'x';
        msgs[i].content[1] = '\0';
        msgs[i].content_len = 1;
        msgs[i].name = NULL;
        msgs[i].name_len = 0;
        msgs[i].tool_call_id = NULL;
        msgs[i].tool_call_id_len = 0;
        msgs[i].tool_calls = NULL;
        msgs[i].tool_calls_count = 0;
    }

    SC_ASSERT_TRUE(sc_should_compact(msgs, 6, &cfg));

    size_t count = 6;
    size_t cap = 6;
    sc_error_t err = sc_compact_history(&alloc, msgs, &count, &cap, &cfg);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count < 6u);
    SC_ASSERT_TRUE(strstr(msgs[1].content, "Compaction") != NULL ||
                   strstr(msgs[1].content, "summary") != NULL);

    for (size_t i = 0; i < count; i++) {
        if (msgs[i].content)
            alloc.free(alloc.ctx, msgs[i].content, msgs[i].content_len + 1);
    }
}

/* ─── reflection ─────────────────────────────────────────────────────────── */

static void test_reflection_evaluate_good(void) {
    sc_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 0, .max_retries = 1};
    sc_reflection_quality_t q =
        sc_reflection_evaluate("What is 2+2?", 11, "The answer is 4.", 16, &cfg);
    SC_ASSERT_EQ(q, SC_QUALITY_GOOD);
}

static void test_reflection_evaluate_needs_retry(void) {
    sc_reflection_config_t cfg = {0};
    sc_reflection_quality_t q = sc_reflection_evaluate("Hi", 2, "x", 1, &cfg);
    SC_ASSERT_EQ(q, SC_QUALITY_NEEDS_RETRY);
}

static void test_reflection_build_critique_prompt(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *prompt = NULL;
    size_t len = 0;
    sc_error_t err = sc_reflection_build_critique_prompt(&alloc, "query", 5, "response", 8,
                                                        &prompt, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prompt);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(strstr(prompt, "query") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "response") != NULL);
    alloc.free(alloc.ctx, prompt, len + 1);
}

/* ─── planner ────────────────────────────────────────────────────────────── */

static void test_planner_create_plan_init_run(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *json =
        "{\"steps\":[{\"tool\":\"file_read\",\"args\":{\"path\":\"/tmp/x\"},\"description\":\"read "
        "file\"},{\"tool\":\"shell\",\"args\":{\"command\":\"echo done\"}}]}";
    sc_plan_t *plan = NULL;
    sc_error_t err = sc_planner_create_plan(&alloc, json, strlen(json), &plan);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(plan);
    SC_ASSERT_EQ(plan->steps_count, 2u);

    sc_plan_step_t *step = sc_planner_next_step(plan);
    SC_ASSERT_NOT_NULL(step);
    SC_ASSERT_STR_EQ(step->tool_name, "file_read");
    SC_ASSERT_EQ(step->status, SC_PLAN_STEP_PENDING);

    sc_planner_mark_step(plan, 0, SC_PLAN_STEP_DONE);
    step = sc_planner_next_step(plan);
    SC_ASSERT_NOT_NULL(step);
    SC_ASSERT_STR_EQ(step->tool_name, "shell");

    sc_planner_mark_step(plan, 1, SC_PLAN_STEP_DONE);
    SC_ASSERT_TRUE(sc_planner_is_complete(plan));
    SC_ASSERT_NULL(sc_planner_next_step(plan));

    sc_plan_free(&alloc, plan);
}

void run_agent_modules_tests(void) {
    SC_TEST_SUITE("agent_modules");

    SC_RUN_TEST(test_outcomes_record_and_retrieve);
    SC_RUN_TEST(test_outcomes_detect_repeated_failure);
    SC_RUN_TEST(test_awareness_init_with_bus);
    SC_RUN_TEST(test_awareness_context_returns_non_null_when_data);
    SC_RUN_TEST(test_awareness_context_null_when_empty);
    SC_RUN_TEST(test_action_preview_generate_simple);
    SC_RUN_TEST(test_episodic_summarize_and_store_recall);
    SC_RUN_TEST(test_memory_loader_null_memory_graceful);
    SC_RUN_TEST(test_context_tokens_lookup_known_model);
    SC_RUN_TEST(test_context_tokens_default);
    SC_RUN_TEST(test_context_tokens_resolve);
    SC_RUN_TEST(test_dispatcher_exists_handles_basic_input);
    SC_RUN_TEST(test_mailbox_send_recv_roundtrip);
    SC_RUN_TEST(test_compaction_message_compaction);
    SC_RUN_TEST(test_reflection_evaluate_good);
    SC_RUN_TEST(test_reflection_evaluate_needs_retry);
    SC_RUN_TEST(test_reflection_build_critique_prompt);
    SC_RUN_TEST(test_planner_create_plan_init_run);
}
