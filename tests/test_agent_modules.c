/* Tests for thin agent modules: outcomes, awareness, action_preview, episodic,
 * memory_loader, context_tokens, dispatcher, mailbox, compaction, reflection, planner. */
#include "human/agent/action_preview.h"
#include "human/agent/awareness.h"
#include "human/agent/compaction.h"
#include "human/agent/constitutional.h"
#include "human/agent/dispatcher.h"
#include "human/agent/episodic.h"
#include "human/agent/mailbox.h"
#include "human/agent/memory_loader.h"
#include "human/agent/outcomes.h"
#include "human/agent/planner.h"
#include "human/agent/reflection.h"
#include "human/agent/tree_of_thought.h"
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

static void test_memory_loader_set_facade_null_loader_noop(void) {
    hu_memory_loader_set_facade(NULL, NULL);
}

static void test_memory_loader_init_clears_facade(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_loader_t loader;
    memset(&loader, 0xAB, sizeof(loader));
    hu_error_t err = hu_memory_loader_init(&loader, &alloc, NULL, NULL, 10, 4000);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(loader.facade);
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
    hu_owned_message_t *hist = msgs;
    hu_error_t err = hu_compact_history(&alloc, &hist, &count, &cap, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count < 6u);
    HU_ASSERT_TRUE(strstr(hist[1].content, "Compaction") != NULL ||
                   strstr(hist[1].content, "summary") != NULL);

    for (size_t i = 0; i < count; i++) {
        if (hist[i].content)
            alloc.free(alloc.ctx, hist[i].content, hist[i].content_len + 1);
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

/* ─── tree_of_thought ───────────────────────────────────────────────────── */

static void test_tot_config_default(void) {
    hu_tot_config_t cfg = hu_tot_config_default();
    HU_ASSERT_EQ(cfg.num_branches, 3);
    HU_ASSERT_EQ(cfg.max_depth, 2);
    HU_ASSERT_TRUE(cfg.prune_threshold >= 0.0 && cfg.prune_threshold <= 1.0);
    HU_ASSERT_TRUE(cfg.enabled);
}

static void test_tot_explore_in_test_mode_returns_mock(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tot_config_t cfg = hu_tot_config_default();
    hu_tot_result_t result;
    memset(&result, 0, sizeof(result));

    /* HU_IS_TEST: no provider needed, returns mock branches with 2-level expansion */
    hu_error_t err = hu_tot_explore(&alloc, NULL, "gpt-4", 4, "Solve X", 7, &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.branches_explored >= 3u);
    HU_ASSERT_TRUE(result.branches_pruned >= 0u);
    /* Best thought should contain "subproblems" (highest scoring branch) */
    if (result.best_thought) {
        HU_ASSERT_TRUE(strstr(result.best_thought, "subproblems") != NULL);
        HU_ASSERT_TRUE(result.best_score > 0.5);
    }

    hu_tot_result_free(&alloc, &result);
}

static void test_tot_result_free_handles_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tot_result_t result;
    memset(&result, 0, sizeof(result));
    hu_tot_result_free(&alloc, &result);
    hu_tot_result_free(NULL, &result);
    hu_tot_result_free(&alloc, NULL);
}

/* ─── constitutional ───────────────────────────────────────────────────── */

static void test_constitutional_config_default(void) {
    hu_constitutional_config_t cfg = hu_constitutional_config_default();
    HU_ASSERT_EQ(cfg.principle_count, 3u);
    HU_ASSERT_TRUE(cfg.enabled);
    HU_ASSERT_TRUE(cfg.rewrite_enabled);
    HU_ASSERT_NOT_NULL(cfg.principles[0].name);
    HU_ASSERT_TRUE(strstr(cfg.principles[0].description, "address") != NULL);
}

static void test_constitutional_critique_in_test_mode_returns_pass(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_constitutional_config_t cfg = hu_constitutional_config_default();
    hu_critique_result_t result;
    memset(&result, 0, sizeof(result));

    /* HU_IS_TEST: no provider needed, always returns PASS */
    hu_error_t err = hu_constitutional_critique(&alloc, NULL, "gpt-4", 4, "Hello", 5, "Hi there", 8,
                                                &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.verdict, HU_CRITIQUE_PASS);
    HU_ASSERT_NULL(result.revised_response);
    HU_ASSERT_EQ(result.principle_index, -1);

    hu_critique_result_free(&alloc, &result);
}

static void test_constitutional_critique_disabled_returns_pass(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_constitutional_config_t cfg = hu_constitutional_config_default();
    cfg.enabled = false;
    cfg.principle_count = 0;
    hu_critique_result_t result;
    memset(&result, 0, sizeof(result));

    hu_error_t err =
        hu_constitutional_critique(&alloc, NULL, "gpt-4", 4, "Hello", 5, "Hi", 2, &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.verdict, HU_CRITIQUE_PASS);

    hu_critique_result_free(&alloc, &result);
}

static void test_critique_result_free_handles_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_critique_result_t result;
    memset(&result, 0, sizeof(result));
    hu_critique_result_free(&alloc, &result);
    hu_critique_result_free(NULL, &result);
    hu_critique_result_free(&alloc, NULL);
}

static void test_parse_verdict_pass(void) {
    int idx = -1;
    HU_ASSERT_EQ(hu_constitutional_test_parse_verdict("PASS - looks good", 18, &idx),
                 HU_CRITIQUE_PASS);
    HU_ASSERT_EQ(idx, -1);
}

static void test_parse_verdict_minor(void) {
    int idx = -1;
    HU_ASSERT_EQ(hu_constitutional_test_parse_verdict("MINOR issue with tone", 21, &idx),
                 HU_CRITIQUE_MINOR);
}

static void test_parse_verdict_rewrite(void) {
    int idx = -1;
    HU_ASSERT_EQ(hu_constitutional_test_parse_verdict("REWRITE: violates principle 2", 29, &idx),
                 HU_CRITIQUE_REWRITE);
}

static void test_parse_verdict_case_insensitive(void) {
    int idx = -1;
    HU_ASSERT_EQ(hu_constitutional_test_parse_verdict("pass", 4, &idx), HU_CRITIQUE_PASS);
    HU_ASSERT_EQ(hu_constitutional_test_parse_verdict("minor", 5, &idx), HU_CRITIQUE_MINOR);
    HU_ASSERT_EQ(hu_constitutional_test_parse_verdict("rewrite", 7, &idx), HU_CRITIQUE_REWRITE);
}

static void test_parse_verdict_empty_returns_pass(void) {
    int idx = 5;
    HU_ASSERT_EQ(hu_constitutional_test_parse_verdict("", 0, &idx), HU_CRITIQUE_PASS);
    HU_ASSERT_EQ(idx, -1);
    HU_ASSERT_EQ(hu_constitutional_test_parse_verdict(NULL, 0, &idx), HU_CRITIQUE_PASS);
}

static void test_parse_verdict_with_leading_whitespace(void) {
    int idx = -1;
    HU_ASSERT_EQ(hu_constitutional_test_parse_verdict("  \n  REWRITE principle 1", 23, &idx),
                 HU_CRITIQUE_REWRITE);
}

static void test_parse_principle_index_finds_number(void) {
    HU_ASSERT_EQ(hu_constitutional_test_parse_principle_index("principle 2", 11), 2);
    HU_ASSERT_EQ(hu_constitutional_test_parse_principle_index("PRINCIPLE 10", 12), 10);
    HU_ASSERT_EQ(hu_constitutional_test_parse_principle_index("violates principle 3 because", 28),
                 3);
}

static void test_parse_principle_index_returns_neg1_when_missing(void) {
    HU_ASSERT_EQ(hu_constitutional_test_parse_principle_index("no number here", 14), -1);
    HU_ASSERT_EQ(hu_constitutional_test_parse_principle_index("", 0), -1);
    HU_ASSERT_EQ(hu_constitutional_test_parse_principle_index(NULL, 0), -1);
}

static void test_parse_principle_index_skips_hash(void) {
    HU_ASSERT_EQ(hu_constitutional_test_parse_principle_index("principle #3 violated", 21), 3);
}

static void test_constitutional_principle_idx_preserved(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_constitutional_config_t cfg = hu_constitutional_config_default();
    hu_critique_result_t result;
    memset(&result, 0, sizeof(result));
    hu_error_t err = hu_constitutional_critique(&alloc, NULL, "test", 4, "Hello", 5, "Response", 8,
                                                &cfg, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.verdict, HU_CRITIQUE_PASS);
    HU_ASSERT_EQ(result.principle_index, -1);
    hu_critique_result_free(&alloc, &result);
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
    HU_RUN_TEST(test_memory_loader_set_facade_null_loader_noop);
    HU_RUN_TEST(test_memory_loader_init_clears_facade);
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
    HU_RUN_TEST(test_tot_config_default);
    HU_RUN_TEST(test_tot_explore_in_test_mode_returns_mock);
    HU_RUN_TEST(test_tot_result_free_handles_null);
    HU_RUN_TEST(test_constitutional_config_default);
    HU_RUN_TEST(test_constitutional_critique_in_test_mode_returns_pass);
    HU_RUN_TEST(test_constitutional_critique_disabled_returns_pass);
    HU_RUN_TEST(test_critique_result_free_handles_null);
    HU_RUN_TEST(test_parse_verdict_pass);
    HU_RUN_TEST(test_parse_verdict_minor);
    HU_RUN_TEST(test_parse_verdict_rewrite);
    HU_RUN_TEST(test_parse_verdict_case_insensitive);
    HU_RUN_TEST(test_parse_verdict_empty_returns_pass);
    HU_RUN_TEST(test_parse_verdict_with_leading_whitespace);
    HU_RUN_TEST(test_parse_principle_index_finds_number);
    HU_RUN_TEST(test_parse_principle_index_returns_neg1_when_missing);
    HU_RUN_TEST(test_parse_principle_index_skips_hash);
    HU_RUN_TEST(test_constitutional_principle_idx_preserved);
}
