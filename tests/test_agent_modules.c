/* Tests for thin agent modules: outcomes, awareness, action_preview, episodic,
 * memory_loader, context_tokens, dispatcher, mailbox, compaction, reflection, planner. */
#include "human/agent/action_preview.h"
#include "human/agent/awareness.h"
#include "human/agent/compaction.h"
#include "human/agent/dispatcher.h"
#include "human/agent/episodic.h"
#include "human/agent/mailbox.h"
#include "human/agent/memory_loader.h"
#include "human/agent/outcomes.h"
#include "human/agent/planner.h"
#include "human/agent/reflection.h"
#include "human/bus.h"
#include "human/config_types.h"
#include "human/context_tokens.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory.h"
#include "test_framework.h"
#include <string.h>

/* ─── outcomes ───────────────────────────────────────────────────────────── */

static void test_outcomes_record_and_retrieve(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);

    hu_outcome_record_tool(&tracker, "file_read", true, "read config");
    hu_outcome_record_tool(&tracker, "shell", false, "command failed");

    size_t count = 0;
    const hu_outcome_entry_t *entries = hu_outcome_get_recent(&tracker, &count);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_EQ(count, 2u);
    HU_ASSERT_EQ(tracker.tool_successes, 1u);
    HU_ASSERT_EQ(tracker.tool_failures, 1u);

    char *summary = hu_outcome_build_summary(&tracker, &alloc, NULL);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(strstr(summary, "succeeded") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "failed") != NULL);
    alloc.free(alloc.ctx, summary, strlen(summary) + 1);
}

static void test_outcomes_detect_repeated_failure(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);

    hu_outcome_record_tool(&tracker, "shell", false, "fail1");
    hu_outcome_record_tool(&tracker, "shell", false, "fail2");
    hu_outcome_record_tool(&tracker, "shell", false, "fail3");

    HU_ASSERT_TRUE(hu_outcome_detect_repeated_failure(&tracker, "shell", 3));
    HU_ASSERT_FALSE(hu_outcome_detect_repeated_failure(&tracker, "shell", 4));
    HU_ASSERT_FALSE(hu_outcome_detect_repeated_failure(&tracker, "file_read", 1));
}

/* ─── awareness ─────────────────────────────────────────────────────────── */

static void test_awareness_init_with_bus(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_error_t err = hu_awareness_init(&aw, &bus);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(aw.state.messages_received, 0u);
    hu_awareness_deinit(&aw);
}

static void test_awareness_context_returns_non_null_when_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    hu_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = HU_BUS_MESSAGE_RECEIVED;
    strncpy(ev.channel, "cli", HU_BUS_CHANNEL_LEN);
    hu_bus_publish(&bus, &ev);

    size_t len = 0;
    char *ctx = hu_awareness_context(&aw, &alloc, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "Situational Awareness") != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);
    hu_awareness_deinit(&aw);
}

static void test_awareness_context_null_when_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    size_t len = 0;
    char *ctx = hu_awareness_context(&aw, &alloc, &len);
    HU_ASSERT_NULL(ctx);
    hu_awareness_deinit(&aw);
}

/* ─── action_preview ────────────────────────────────────────────────────── */

static void test_action_preview_generate_simple(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_action_preview_t p;
    const char *args = "{\"path\":\"/tmp/test.txt\"}";
    hu_error_t err = hu_action_preview_generate(&alloc, "file_read", args, strlen(args), &p);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(p.description);
    HU_ASSERT_TRUE(strstr(p.description, "/tmp/test.txt") != NULL);
    HU_ASSERT_STR_EQ(p.risk_level, "low");

    char *formatted = NULL;
    size_t fmt_len = 0;
    err = hu_action_preview_format(&alloc, &p, &formatted, &fmt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(formatted);
    HU_ASSERT_TRUE(strstr(formatted, "file_read") != NULL);
    alloc.free(alloc.ctx, formatted, fmt_len + 1);
    hu_action_preview_free(&alloc, &p);
}

/* ─── episodic ───────────────────────────────────────────────────────────── */

static void test_episodic_summarize_and_store_recall(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"What is the config format?", "Here is the config..."};
    size_t lens[] = {strlen(msgs[0]), strlen(msgs[1])};
    size_t out_len = 0;

    char *summary = hu_episodic_summarize_session(&alloc, msgs, lens, 2, &out_len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(summary, "config format") != NULL);

#ifdef HU_ENABLE_SQLITE
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.ctx);
#else
    hu_memory_t mem = hu_none_memory_create(&alloc);
    HU_ASSERT_NOT_NULL(mem.ctx);
#endif

    hu_error_t err = hu_episodic_store(&mem, &alloc, "sess_1", 6, summary, out_len);
    HU_ASSERT_EQ(err, HU_OK);
    alloc.free(alloc.ctx, summary, out_len + 1);

    char *loaded = NULL;
    size_t loaded_len = 0;
    err = hu_episodic_load(&mem, &alloc, &loaded, &loaded_len);
    HU_ASSERT_EQ(err, HU_OK);
#ifdef HU_ENABLE_SQLITE
    HU_ASSERT_NOT_NULL(loaded);
    HU_ASSERT_TRUE(loaded_len > 0);
    HU_ASSERT_TRUE(strstr(loaded, "config format") != NULL);
    alloc.free(alloc.ctx, loaded, loaded_len + 1);
#else
    HU_ASSERT_NULL(loaded);
#endif
    mem.vtable->deinit(mem.ctx);
}

/* ─── memory_loader ──────────────────────────────────────────────────────── */

static void test_memory_loader_null_memory_graceful(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_loader_t loader;
    hu_error_t err = hu_memory_loader_init(&loader, &alloc, NULL, NULL, 10, 4000);
    HU_ASSERT_EQ(err, HU_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    err = hu_memory_loader_load(&loader, "query", 5, NULL, 0, &ctx, &ctx_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(ctx_len, 0u);
}

/* ─── context_tokens ────────────────────────────────────────────────────── */

static void test_context_tokens_lookup_known_model(void) {
    uint64_t v = hu_context_tokens_lookup("claude-sonnet-4.6", 17);
    HU_ASSERT_EQ(v, 200000u);

    v = hu_context_tokens_lookup("gpt-4.1", 7);
    HU_ASSERT_EQ(v, 128000u);
}

static void test_context_tokens_default(void) {
    uint64_t v = hu_context_tokens_default();
    HU_ASSERT_EQ(v, HU_DEFAULT_AGENT_TOKEN_LIMIT);
}

static void test_context_tokens_resolve(void) {
    uint64_t v = hu_context_tokens_resolve(50000, "unknown", 7);
    HU_ASSERT_EQ(v, 50000u);

    v = hu_context_tokens_resolve(0, "claude-opus-4-6", 14);
    HU_ASSERT_EQ(v, 200000u);
}

/* ─── dispatcher ─────────────────────────────────────────────────────────── */

static void test_dispatcher_exists_handles_basic_input(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_dispatcher_t disp;
    hu_dispatcher_default(&disp);
    HU_ASSERT_EQ(disp.max_parallel, 1u);

    hu_tool_call_t calls[1] = {{.id = "x",
                                .id_len = 1,
                                .name = "nonexistent",
                                .name_len = 10,
                                .arguments = "{}",
                                .arguments_len = 2}};
    hu_dispatch_result_t out;
    hu_error_t err = hu_dispatcher_dispatch(&disp, &alloc, NULL, 0, calls, 1, &out);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(out.count, 1u);
    HU_ASSERT_NOT_NULL(out.results);
    HU_ASSERT_FALSE(out.results[0].success);
    hu_dispatch_result_free(&alloc, &out);
}

/* ─── mailbox ────────────────────────────────────────────────────────────── */

static void test_mailbox_send_recv_roundtrip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_mailbox_t *mbox = hu_mailbox_create(&alloc, 4);
    HU_ASSERT_NOT_NULL(mbox);

    hu_error_t err = hu_mailbox_register(mbox, 1);
    HU_ASSERT_EQ(err, HU_OK);
    err = hu_mailbox_register(mbox, 2);
    HU_ASSERT_EQ(err, HU_OK);

    const char *payload = "hello";
    err = hu_mailbox_send(mbox, 1, 2, HU_MSG_TASK, payload, strlen(payload), 0);
    HU_ASSERT_EQ(err, HU_OK);

    hu_message_t msg;
    err = hu_mailbox_recv(mbox, 2, &msg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(msg.type, HU_MSG_TASK);
    HU_ASSERT_EQ(msg.from_agent, 1u);
    HU_ASSERT_EQ(msg.to_agent, 2u);
    HU_ASSERT_NOT_NULL(msg.payload);
    HU_ASSERT_STR_EQ(msg.payload, "hello");
    hu_message_free(&alloc, &msg);

    HU_ASSERT_EQ(hu_mailbox_pending_count(mbox, 2), 0u);
    hu_mailbox_destroy(mbox);
}

/* ─── compaction ─────────────────────────────────────────────────────────── */

static void test_compaction_message_compaction(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_compaction_config_t cfg;
    hu_compaction_config_default(&cfg);
    cfg.keep_recent = 2;
    cfg.max_history_messages = 4;

    hu_owned_message_t msgs[6];
    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < 6; i++) {
        msgs[i].role = i == 0 ? HU_ROLE_SYSTEM : HU_ROLE_USER;
        msgs[i].content = alloc.alloc(alloc.ctx, 2);
        HU_ASSERT_NOT_NULL(msgs[i].content);
        msgs[i].content[0] = 'x';
        msgs[i].content[1] = '\0';
        msgs[i].content_len = 1;
    }

    HU_ASSERT_TRUE(hu_should_compact(msgs, 6, &cfg));

    size_t count = 6;
    size_t cap = 6;
    hu_error_t err = hu_compact_history(&alloc, msgs, &count, &cap, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count < 6u);
    HU_ASSERT_TRUE(strstr(msgs[1].content, "Compaction") != NULL ||
                   strstr(msgs[1].content, "summary") != NULL);

    for (size_t i = 0; i < count; i++) {
        if (msgs[i].content)
            alloc.free(alloc.ctx, msgs[i].content, msgs[i].content_len + 1);
    }
}

/* ─── reflection ─────────────────────────────────────────────────────────── */

static void test_reflection_evaluate_good(void) {
    hu_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 0, .max_retries = 1};
    hu_reflection_quality_t q =
        hu_reflection_evaluate("What is 2+2?", 11, "The answer is 4.", 16, &cfg);
    HU_ASSERT_EQ(q, HU_QUALITY_GOOD);
}

static void test_reflection_evaluate_needs_retry(void) {
    hu_reflection_config_t cfg = {0};
    hu_reflection_quality_t q = hu_reflection_evaluate("Hi", 2, "x", 1, &cfg);
    HU_ASSERT_EQ(q, HU_QUALITY_NEEDS_RETRY);
}

static void test_reflection_build_critique_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *prompt = NULL;
    size_t len = 0;
    hu_error_t err =
        hu_reflection_build_critique_prompt(&alloc, "query", 5, "response", 8, &prompt, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(prompt, "query") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "response") != NULL);
    alloc.free(alloc.ctx, prompt, len + 1);
}

/* ─── planner ────────────────────────────────────────────────────────────── */

static void test_planner_create_plan_init_run(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json =
        "{\"steps\":[{\"tool\":\"file_read\",\"args\":{\"path\":\"/tmp/x\"},\"description\":\"read "
        "file\"},{\"tool\":\"shell\",\"args\":{\"command\":\"echo done\"}}]}";
    hu_plan_t *plan = NULL;
    hu_error_t err = hu_planner_create_plan(&alloc, json, strlen(json), &plan);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(plan);
    HU_ASSERT_EQ(plan->steps_count, 2u);

    hu_plan_step_t *step = hu_planner_next_step(plan);
    HU_ASSERT_NOT_NULL(step);
    HU_ASSERT_STR_EQ(step->tool_name, "file_read");
    HU_ASSERT_EQ(step->status, HU_PLAN_STEP_PENDING);

    hu_planner_mark_step(plan, 0, HU_PLAN_STEP_DONE);
    step = hu_planner_next_step(plan);
    HU_ASSERT_NOT_NULL(step);
    HU_ASSERT_STR_EQ(step->tool_name, "shell");

    hu_planner_mark_step(plan, 1, HU_PLAN_STEP_DONE);
    HU_ASSERT_TRUE(hu_planner_is_complete(plan));
    HU_ASSERT_NULL(hu_planner_next_step(plan));

    hu_plan_free(&alloc, plan);
}

void run_agent_modules_tests(void) {
    HU_TEST_SUITE("agent_modules");

    HU_RUN_TEST(test_outcomes_record_and_retrieve);
    HU_RUN_TEST(test_outcomes_detect_repeated_failure);
    HU_RUN_TEST(test_awareness_init_with_bus);
    HU_RUN_TEST(test_awareness_context_returns_non_null_when_data);
    HU_RUN_TEST(test_awareness_context_null_when_empty);
    HU_RUN_TEST(test_action_preview_generate_simple);
    HU_RUN_TEST(test_episodic_summarize_and_store_recall);
    HU_RUN_TEST(test_memory_loader_null_memory_graceful);
    HU_RUN_TEST(test_context_tokens_lookup_known_model);
    HU_RUN_TEST(test_context_tokens_default);
    HU_RUN_TEST(test_context_tokens_resolve);
    HU_RUN_TEST(test_dispatcher_exists_handles_basic_input);
    HU_RUN_TEST(test_mailbox_send_recv_roundtrip);
    HU_RUN_TEST(test_compaction_message_compaction);
    HU_RUN_TEST(test_reflection_evaluate_good);
    HU_RUN_TEST(test_reflection_evaluate_needs_retry);
    HU_RUN_TEST(test_reflection_build_critique_prompt);
    HU_RUN_TEST(test_planner_create_plan_init_run);
}
