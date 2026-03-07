#include "seaclaw/agent/awareness.h"
#include "seaclaw/agent/compaction.h"
#include "seaclaw/agent/outcomes.h"
#include "seaclaw/agent/planner.h"
#include "seaclaw/agent/episodic.h"
#include "seaclaw/agent/preferences.h"
#include "seaclaw/agent/prompt.h"
#include "seaclaw/agent/reflection.h"
#include "seaclaw/bus.h"
#include "seaclaw/cron.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

/* ── Tone detection ─────────────────────────────────────────────────────── */

static void test_tone_neutral(void) {
    const char *msgs[] = {"Hello, can you help me?"};
    size_t lens[] = {strlen(msgs[0])};
    sc_tone_t t = sc_detect_tone(msgs, lens, 1);
    SC_ASSERT_EQ(t, SC_TONE_NEUTRAL);
}

static void test_tone_casual(void) {
    const char *msgs[] = {"lol hey!", "omg that's cool!", "haha nice!"};
    size_t lens[] = {strlen(msgs[0]), strlen(msgs[1]), strlen(msgs[2])};
    sc_tone_t t = sc_detect_tone(msgs, lens, 3);
    SC_ASSERT_EQ(t, SC_TONE_CASUAL);
}

static void test_tone_technical(void) {
    const char *msgs[] = {"I'm getting a stack trace error in src/main.c",
                          "The config debug output shows `/etc/app.conf`",
                          "Here is the error ```segfault```"};
    size_t lens[] = {strlen(msgs[0]), strlen(msgs[1]), strlen(msgs[2])};
    sc_tone_t t = sc_detect_tone(msgs, lens, 3);
    SC_ASSERT_EQ(t, SC_TONE_TECHNICAL);
}

static void test_tone_formal(void) {
    const char *msgs[] = {
        "I would like to request a detailed analysis of the performance characteristics "
        "of the current implementation across various hardware configurations.",
        "Please provide a comprehensive report of the findings and any recommendations "
        "for improvement that you deem necessary.",
        "Additionally, could you include a summary of the methodology used in the analysis "
        "and any limitations that should be noted."};
    size_t lens[] = {strlen(msgs[0]), strlen(msgs[1]), strlen(msgs[2])};
    sc_tone_t t = sc_detect_tone(msgs, lens, 3);
    SC_ASSERT_EQ(t, SC_TONE_FORMAL);
}

static void test_tone_hint_string(void) {
    size_t len = 0;
    const char *s = sc_tone_hint_string(SC_TONE_CASUAL, &len);
    SC_ASSERT_NOT_NULL(s);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(strstr(s, "casually") != NULL);

    s = sc_tone_hint_string(SC_TONE_NEUTRAL, &len);
    SC_ASSERT_NULL(s);
    SC_ASSERT_EQ(len, 0);
}

static void test_tone_null_input(void) {
    sc_tone_t t = sc_detect_tone(NULL, NULL, 0);
    SC_ASSERT_EQ(t, SC_TONE_NEUTRAL);
}

/* ── Preferences ────────────────────────────────────────────────────────── */

static void test_pref_is_correction_positive(void) {
    SC_ASSERT_TRUE(sc_preferences_is_correction("No, I want JSON output", 22));
    SC_ASSERT_TRUE(sc_preferences_is_correction("actually, use tabs", 18));
    SC_ASSERT_TRUE(sc_preferences_is_correction("I prefer dark mode", 18));
    SC_ASSERT_TRUE(sc_preferences_is_correction("Don't use emojis", 16));
    SC_ASSERT_TRUE(sc_preferences_is_correction("Stop using semicolons", 21));
    SC_ASSERT_TRUE(sc_preferences_is_correction("Never add comments", 18));
    SC_ASSERT_TRUE(sc_preferences_is_correction("Always use const", 16));
    SC_ASSERT_TRUE(sc_preferences_is_correction("Instead, return an error", 24));
}

static void test_pref_is_correction_negative(void) {
    SC_ASSERT_FALSE(sc_preferences_is_correction("What is rust?", 13));
    SC_ASSERT_FALSE(sc_preferences_is_correction("Please explain this", 19));
    SC_ASSERT_FALSE(sc_preferences_is_correction("How do I compile?", 17));
    SC_ASSERT_FALSE(sc_preferences_is_correction(NULL, 0));
    SC_ASSERT_FALSE(sc_preferences_is_correction("ab", 2));
}

static void test_pref_extract(void) {
    sc_allocator_t alloc = sc_system_allocator();
    size_t len = 0;
    char *pref = sc_preferences_extract(&alloc, "  Always use tabs  ", 19, &len);
    SC_ASSERT_NOT_NULL(pref);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(strstr(pref, "Always") != NULL);
    alloc.free(alloc.ctx, pref, len + 1);
}

static void test_pref_extract_null(void) {
    size_t len = 0;
    char *pref = sc_preferences_extract(NULL, "test", 4, &len);
    SC_ASSERT_NULL(pref);
}

/* ── Reflection ─────────────────────────────────────────────────────────── */

static void test_reflection_empty_response(void) {
    sc_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 0, .max_retries = 1};
    sc_reflection_quality_t q = sc_reflection_evaluate("hello?", 6, "", 0, &cfg);
    SC_ASSERT_EQ(q, SC_QUALITY_NEEDS_RETRY);
}

static void test_reflection_short_response(void) {
    sc_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 0, .max_retries = 1};
    sc_reflection_quality_t q = sc_reflection_evaluate("hello?", 6, "Hi", 2, &cfg);
    SC_ASSERT_EQ(q, SC_QUALITY_NEEDS_RETRY);
}

static void test_reflection_good_response(void) {
    sc_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 0, .max_retries = 1};
    const char *resp = "Here is a detailed answer to your question about the configuration "
                       "settings and how they interact with the runtime environment.";
    sc_reflection_quality_t q =
        sc_reflection_evaluate("How does config work?", 21, resp, strlen(resp), &cfg);
    SC_ASSERT_EQ(q, SC_QUALITY_GOOD);
}

static void test_reflection_refusal_response(void) {
    sc_reflection_config_t cfg = {.enabled = true};
    const char *resp = "I cannot help with that request as an AI.";
    sc_reflection_quality_t q =
        sc_reflection_evaluate("do something", 12, resp, strlen(resp), &cfg);
    SC_ASSERT_EQ(q, SC_QUALITY_ACCEPTABLE);
}

static void test_reflection_build_critique(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *prompt = NULL;
    size_t prompt_len = 0;
    sc_error_t err = sc_reflection_build_critique_prompt(&alloc, "How?", 4, "Like this.", 10,
                                                         &prompt, &prompt_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prompt);
    SC_ASSERT_TRUE(strstr(prompt, "Evaluate") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "How?") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "Like this.") != NULL);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
}

static void test_reflection_result_free(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_reflection_result_t r = {.quality = SC_QUALITY_GOOD, .feedback = NULL, .feedback_len = 0};
    sc_reflection_result_free(&alloc, &r);
    sc_reflection_result_free(NULL, NULL);
}

/* ── Episodic memory ────────────────────────────────────────────────────── */

static void test_episodic_summarize_basic(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *msgs[] = {"How do I configure the logger?", "Use sc_log_init with level param."};
    size_t lens[] = {strlen(msgs[0]), strlen(msgs[1])};
    size_t out_len = 0;
    char *summary = sc_episodic_summarize_session(&alloc, msgs, lens, 2, &out_len);
    SC_ASSERT_NOT_NULL(summary);
    SC_ASSERT_TRUE(out_len > 0);
    SC_ASSERT_TRUE(strstr(summary, "Session topic:") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "configure") != NULL);
    alloc.free(alloc.ctx, summary, out_len + 1);
}

static void test_episodic_summarize_null(void) {
    char *summary = sc_episodic_summarize_session(NULL, NULL, NULL, 0, NULL);
    SC_ASSERT_NULL(summary);
}

static void test_episodic_load_null_out(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    size_t out_len = 0;
    sc_error_t err = sc_episodic_load(&mem, &alloc, NULL, &out_len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_episodic_load_null_alloc(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_episodic_load(&mem, NULL, &out, &out_len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_episodic_store_null_memory(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_error_t err = sc_episodic_store(NULL, &alloc, "s", 1, "summary", 7);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_episodic_store_null_summary(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_none_memory_create(&alloc);
    sc_error_t err = sc_episodic_store(&mem, &alloc, "s", 1, NULL, 0);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void test_episodic_summarize_null_messages(void) {
    sc_allocator_t alloc = sc_system_allocator();
    size_t lens[] = {5};
    size_t out_len = 0;
    char *summary = sc_episodic_summarize_session(&alloc, NULL, lens, 1, &out_len);
    SC_ASSERT_NULL(summary);
}

static void test_episodic_summarize_zero_count(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *msgs[] = {"hello"};
    size_t lens[] = {5};
    size_t out_len = 0;
    char *summary = sc_episodic_summarize_session(&alloc, msgs, lens, 0, &out_len);
    SC_ASSERT_NULL(summary);
}

#ifdef SC_ENABLE_SQLITE
static void test_episodic_store_load_sqlite(void) {
    sc_allocator_t alloc = sc_system_allocator();

    sc_memory_t mem = sc_sqlite_memory_create(&alloc, ":memory:");
    SC_ASSERT_NOT_NULL(mem.vtable);
    SC_ASSERT_NOT_NULL(mem.ctx);

    sc_error_t err =
        sc_episodic_store(&mem, &alloc, "session_abc", 11, "User asked about config parsing", 31);
    SC_ASSERT_EQ(err, SC_OK);

    char *out = NULL;
    size_t out_len = 0;
    err = sc_episodic_load(&mem, &alloc, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(out_len > 0);
    SC_ASSERT_TRUE(strstr(out, "config parsing") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    mem.vtable->deinit(mem.ctx);
}
#endif

/* ── Awareness ──────────────────────────────────────────────────────────── */

static void test_awareness_init_deinit(void) {
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_awareness_t aw;
    sc_error_t err = sc_awareness_init(&aw, &bus);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(aw.state.messages_received, 0);
    sc_awareness_deinit(&aw);
}

static void test_awareness_tracks_messages(void) {
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_awareness_t aw;
    sc_awareness_init(&aw, &bus);

    sc_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SC_BUS_MESSAGE_RECEIVED;
    strncpy(ev.channel, "cli", SC_BUS_CHANNEL_LEN);
    sc_bus_publish(&bus, &ev);

    SC_ASSERT_EQ(aw.state.messages_received, 1);
    SC_ASSERT_EQ(aw.state.active_channel_count, 1);

    ev.type = SC_BUS_TOOL_CALL;
    sc_bus_publish(&bus, &ev);
    SC_ASSERT_EQ(aw.state.tool_calls, 1);

    sc_awareness_deinit(&aw);
}

static void test_awareness_tracks_errors(void) {
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_awareness_t aw;
    sc_awareness_init(&aw, &bus);

    sc_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SC_BUS_ERROR;
    strncpy(ev.message, "provider timeout", SC_BUS_MSG_LEN);
    sc_bus_publish(&bus, &ev);

    SC_ASSERT_EQ(aw.state.total_errors, 1);
    SC_ASSERT_TRUE(strstr(aw.state.recent_errors[0].text, "provider timeout") != NULL);

    sc_awareness_deinit(&aw);
}

static void test_awareness_context_empty(void) {
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

static void test_awareness_context_with_data(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_bus_t bus;
    sc_bus_init(&bus);
    sc_awareness_t aw;
    sc_awareness_init(&aw, &bus);

    sc_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = SC_BUS_MESSAGE_RECEIVED;
    strncpy(ev.channel, "telegram", SC_BUS_CHANNEL_LEN);
    sc_bus_publish(&bus, &ev);

    ev.type = SC_BUS_ERROR;
    strncpy(ev.message, "test error", SC_BUS_MSG_LEN);
    sc_bus_publish(&bus, &ev);

    size_t len = 0;
    char *ctx = sc_awareness_context(&aw, &alloc, &len);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(strstr(ctx, "Situational Awareness") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "telegram") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "test error") != NULL);

    alloc.free(alloc.ctx, ctx, len + 1);
    sc_awareness_deinit(&aw);
}

/* ── Prompt with new fields ─────────────────────────────────────────────── */

static void test_prompt_with_persona(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_prompt_config_t cfg = {
        .provider_name = "openai",
        .provider_name_len = 6,
        .model_name = "gpt-4",
        .model_name_len = 5,
        .workspace_dir = "/tmp",
        .workspace_dir_len = 4,
        .autonomy_level = 2,
        .persona_prompt = "You are Jarvis, a witty butler AI.",
        .persona_prompt_len = 34,
        .chain_of_thought = true,
    };

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_prompt_build_system(&alloc, &cfg, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(strstr(out, "Jarvis") != NULL);
    SC_ASSERT_TRUE(strstr(out, "SeaClaw") == NULL);
    SC_ASSERT_TRUE(strstr(out, "Reasoning") != NULL);
    SC_ASSERT_TRUE(strstr(out, "step by step") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_with_tone_and_prefs(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_prompt_config_t cfg = {
        .provider_name = "openai",
        .provider_name_len = 6,
        .model_name = "gpt-4",
        .model_name_len = 5,
        .workspace_dir = "/tmp",
        .workspace_dir_len = 4,
        .autonomy_level = 1,
        .tone_hint = "Match the user's casual tone.",
        .tone_hint_len = 29,
        .preferences = "- Always use snake_case\n- Prefer const\n",
        .preferences_len = 39,
    };

    char *out = NULL;
    size_t out_len = 0;
    sc_error_t err = sc_prompt_build_system(&alloc, &cfg, &out, &out_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(out);
    SC_ASSERT_TRUE(strstr(out, "Tone") != NULL);
    SC_ASSERT_TRUE(strstr(out, "casual") != NULL);
    SC_ASSERT_TRUE(strstr(out, "User Preferences") != NULL);
    SC_ASSERT_TRUE(strstr(out, "snake_case") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ── LLM compaction (provider=NULL fallback) ────────────────────────────── */

static void test_compaction_llm_fallback(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_compaction_config_t cfg;
    sc_compaction_config_default(&cfg);
    cfg.keep_recent = 2;
    cfg.max_history_messages = 3;

    sc_owned_message_t msgs[5];
    memset(msgs, 0, sizeof(msgs));

    msgs[0].role = SC_ROLE_SYSTEM;
    msgs[0].content = sc_strndup(&alloc, "System prompt", 13);
    msgs[0].content_len = 13;

    for (int i = 1; i < 5; i++) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "Message number %d", i);
        msgs[i].role = (i % 2 == 1) ? SC_ROLE_USER : SC_ROLE_ASSISTANT;
        msgs[i].content = sc_strndup(&alloc, buf, (size_t)n);
        msgs[i].content_len = (size_t)n;
    }

    size_t count = 5;
    size_t cap = 5;

    sc_error_t err = sc_compact_history(&alloc, msgs, &count, &cap, &cfg);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(count < 5);

    for (size_t i = 0; i < count; i++) {
        if (msgs[i].content)
            alloc.free(alloc.ctx, msgs[i].content, msgs[i].content_len + 1);
        if (msgs[i].name)
            alloc.free(alloc.ctx, msgs[i].name, msgs[i].name_len + 1);
        if (msgs[i].tool_call_id)
            alloc.free(alloc.ctx, msgs[i].tool_call_id, msgs[i].tool_call_id_len + 1);
    }
}

/* ── Upgrade 1: Awareness prompt injection ──────────────────────────── */

static void test_awareness_prompt_injection(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.awareness_context = "## Situational Awareness\n- Session stats: 5 msgs received\n";
    cfg.awareness_context_len = strlen(cfg.awareness_context);

    char *prompt = NULL;
    size_t prompt_len = 0;
    sc_error_t err = sc_prompt_build_system(&alloc, &cfg, &prompt, &prompt_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(prompt);
    SC_ASSERT_TRUE(strstr(prompt, "Situational Awareness") != NULL);
    SC_ASSERT_TRUE(strstr(prompt, "5 msgs received") != NULL);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
}

static void test_awareness_prompt_null_skipped(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    char *prompt = NULL;
    size_t prompt_len = 0;
    sc_error_t err = sc_prompt_build_system(&alloc, &cfg, &prompt, &prompt_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(strstr(prompt, "Situational Awareness") == NULL);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
}

/* ── Upgrade 2: Agent cron jobs ─────────────────────────────────────── */

static void test_cron_add_agent_job(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *sched = sc_cron_create(&alloc, 64, true);
    SC_ASSERT_NOT_NULL(sched);

    uint64_t id = 0;
    sc_error_t err = sc_cron_add_agent_job(sched, &alloc, "0 8 * * *",
                                           "Check my calendar and send a daily brief", "slack",
                                           "daily-brief", &id);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(id > 0);

    const sc_cron_job_t *job = sc_cron_get_job(sched, id);
    SC_ASSERT_NOT_NULL(job);
    SC_ASSERT_EQ(job->type, SC_CRON_JOB_AGENT);
    SC_ASSERT_NOT_NULL(job->channel);
    SC_ASSERT_TRUE(strcmp(job->channel, "slack") == 0);
    SC_ASSERT_TRUE(strcmp(job->command, "Check my calendar and send a daily brief") == 0);

    sc_cron_destroy(sched, &alloc);
}

static void test_cron_add_agent_job_null_prompt(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *sched = sc_cron_create(&alloc, 64, true);
    uint64_t id = 0;
    sc_error_t err = sc_cron_add_agent_job(sched, &alloc, "* * * * *", NULL, NULL, NULL, &id);
    SC_ASSERT_NEQ(err, SC_OK);
    sc_cron_destroy(sched, &alloc);
}

static void test_cron_shell_job_type_default(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_cron_scheduler_t *sched = sc_cron_create(&alloc, 64, true);
    uint64_t id = 0;
    sc_error_t err = sc_cron_add_job(sched, &alloc, "* * * * *", "echo hello", NULL, &id);
    SC_ASSERT_EQ(err, SC_OK);
    const sc_cron_job_t *job = sc_cron_get_job(sched, id);
    SC_ASSERT_NOT_NULL(job);
    SC_ASSERT_EQ(job->type, SC_CRON_JOB_SHELL);
    sc_cron_destroy(sched, &alloc);
}

/* ── Upgrade 3: Reflection retry ────────────────────────────────────── */

static void test_reflection_needs_retry_on_empty(void) {
    sc_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 0, .max_retries = 2};
    sc_reflection_quality_t q = sc_reflection_evaluate("hello?", 6, "", 0, &cfg);
    SC_ASSERT_EQ(q, SC_QUALITY_NEEDS_RETRY);
}

static void test_reflection_good_after_long_response(void) {
    sc_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 0, .max_retries = 2};
    const char *resp = "This is a comprehensive answer that thoroughly addresses the question "
                       "about configuration settings and runtime behavior.";
    sc_reflection_quality_t q = sc_reflection_evaluate("How does config?", 16, resp, strlen(resp), &cfg);
    SC_ASSERT_EQ(q, SC_QUALITY_GOOD);
}

static void test_reflection_critique_prompt_format(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *critique = NULL;
    size_t len = 0;
    sc_error_t err = sc_reflection_build_critique_prompt(&alloc, "what is 2+2?", 12, "idk", 3,
                                                         &critique, &len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(critique);
    SC_ASSERT_TRUE(strstr(critique, "what is 2+2?") != NULL);
    SC_ASSERT_TRUE(strstr(critique, "idk") != NULL);
    alloc.free(alloc.ctx, critique, len + 1);
}

/* ── Upgrade 4: Plan generation ─────────────────────────────────────── */

static void test_planner_generate_stub(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_plan_t *plan = NULL;
    const char *tools[] = {"shell", "file_read", "web_search"};

    /* In test mode, sc_planner_generate returns a stub plan */
    sc_provider_vtable_t dummy_vtable;
    memset(&dummy_vtable, 0, sizeof(dummy_vtable));
    sc_provider_t dummy_provider = {.vtable = &dummy_vtable, .ctx = NULL};

    sc_error_t err = sc_planner_generate(&alloc, &dummy_provider, "gpt-4o", 6,
                                         "Find all TODO comments in the codebase", 39, tools, 3,
                                         &plan);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(plan);
    SC_ASSERT_TRUE(plan->steps_count > 0);
    SC_ASSERT_NOT_NULL(plan->steps[0].tool_name);
    sc_plan_free(&alloc, plan);
}

static void test_planner_generate_null_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_plan_t *plan = NULL;
    sc_error_t err = sc_planner_generate(NULL, NULL, NULL, 0, NULL, 0, NULL, 0, &plan);
    SC_ASSERT_NEQ(err, SC_OK);
    err = sc_planner_generate(&alloc, NULL, NULL, 0, "test", 4, NULL, 0, &plan);
    SC_ASSERT_NEQ(err, SC_OK);
}

/* ── Upgrade 5: Outcome tracking ─────────────────────────────────────── */

static void test_outcome_tracker_init(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, true);
    SC_ASSERT_EQ(tracker.total, 0);
    SC_ASSERT_EQ(tracker.tool_successes, 0);
    SC_ASSERT_EQ(tracker.tool_failures, 0);
    SC_ASSERT_EQ(tracker.corrections, 0);
    SC_ASSERT_EQ(tracker.positives, 0);
    SC_ASSERT_TRUE(tracker.auto_apply_feedback);
}

static void test_outcome_record_tool_success(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_tool(&tracker, "shell", true, "executed echo hello");
    SC_ASSERT_EQ(tracker.total, 1);
    SC_ASSERT_EQ(tracker.tool_successes, 1);
    SC_ASSERT_EQ(tracker.tool_failures, 0);
}

static void test_outcome_record_tool_failure(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_tool(&tracker, "shell", false, "command not found");
    SC_ASSERT_EQ(tracker.total, 1);
    SC_ASSERT_EQ(tracker.tool_successes, 0);
    SC_ASSERT_EQ(tracker.tool_failures, 1);
}

static void test_outcome_record_correction(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_correction(&tracker, "I said X", "no, actually Y");
    SC_ASSERT_EQ(tracker.total, 1);
    SC_ASSERT_EQ(tracker.corrections, 1);
}

static void test_outcome_record_positive(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_positive(&tracker, "thanks, that's great");
    SC_ASSERT_EQ(tracker.total, 1);
    SC_ASSERT_EQ(tracker.positives, 1);
}

static void test_outcome_get_recent(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_tool(&tracker, "shell", true, "ok");
    sc_outcome_record_tool(&tracker, "web_search", false, "timeout");
    size_t count = 0;
    const sc_outcome_entry_t *entries = sc_outcome_get_recent(&tracker, &count);
    SC_ASSERT_NOT_NULL(entries);
    SC_ASSERT_EQ(count, 2);
}

static void test_outcome_build_summary(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);

    char *summary = sc_outcome_build_summary(&tracker, &alloc, NULL);
    SC_ASSERT_NULL(summary); /* empty tracker returns NULL */

    sc_outcome_record_tool(&tracker, "shell", true, "ok");
    sc_outcome_record_tool(&tracker, "web_search", false, "fail");
    sc_outcome_record_correction(&tracker, NULL, "actually do X");

    size_t len = 0;
    summary = sc_outcome_build_summary(&tracker, &alloc, &len);
    SC_ASSERT_NOT_NULL(summary);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(strstr(summary, "1 succeeded") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "1 failed") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "corrections: 1") != NULL);
    alloc.free(alloc.ctx, summary, len + 1);
}

static void test_outcome_detect_repeated_failure(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    SC_ASSERT_FALSE(sc_outcome_detect_repeated_failure(&tracker, "shell", 3));
    sc_outcome_record_tool(&tracker, "shell", false, "err1");
    sc_outcome_record_tool(&tracker, "shell", false, "err2");
    SC_ASSERT_FALSE(sc_outcome_detect_repeated_failure(&tracker, "shell", 3));
    sc_outcome_record_tool(&tracker, "shell", false, "err3");
    SC_ASSERT_TRUE(sc_outcome_detect_repeated_failure(&tracker, "shell", 3));
}

static void test_outcome_circular_buffer(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    for (size_t i = 0; i < SC_OUTCOME_MAX_RECENT + 10; i++)
        sc_outcome_record_tool(&tracker, "shell", true, "ok");
    SC_ASSERT_EQ(tracker.total, SC_OUTCOME_MAX_RECENT + 10);
    size_t count = 0;
    (void)sc_outcome_get_recent(&tracker, &count);
    SC_ASSERT_EQ(count, SC_OUTCOME_MAX_RECENT);
}

static void test_outcome_null_safety(void) {
    sc_outcome_record_tool(NULL, "shell", true, "ok");
    sc_outcome_record_correction(NULL, NULL, NULL);
    sc_outcome_record_positive(NULL, NULL);
    SC_ASSERT_FALSE(sc_outcome_detect_repeated_failure(NULL, "shell", 1));
    size_t count = 0;
    SC_ASSERT_NULL(sc_outcome_get_recent(NULL, &count));
}

static void test_outcome_tracker_init_null(void) {
    sc_outcome_tracker_init(NULL, true);
    /* no-op, should not crash */
}

static void test_outcome_get_recent_null_count(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_tool(&tracker, "shell", true, "ok");
    SC_ASSERT_NULL(sc_outcome_get_recent(&tracker, NULL));
}

static void test_outcome_build_summary_null_alloc(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_tool(&tracker, "shell", true, "ok");
    SC_ASSERT_NULL(sc_outcome_build_summary(&tracker, NULL, NULL));
}

static void test_outcome_build_summary_null_tracker(void) {
    sc_allocator_t alloc = sc_system_allocator();
    SC_ASSERT_NULL(sc_outcome_build_summary(NULL, &alloc, NULL));
}

static void test_outcome_detect_repeated_failure_null_tool_name(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    SC_ASSERT_FALSE(sc_outcome_detect_repeated_failure(&tracker, NULL, 1));
}

static void test_outcome_detect_repeated_failure_zero_threshold(void) {
    sc_outcome_tracker_t tracker;
    sc_outcome_tracker_init(&tracker, false);
    sc_outcome_record_tool(&tracker, "shell", false, "err");
    SC_ASSERT_FALSE(sc_outcome_detect_repeated_failure(&tracker, "shell", 0));
}

/* ── Run all ────────────────────────────────────────────────────────────── */

void run_intelligence_tests(void) {
    SC_TEST_SUITE("Intelligence Enhancement");

    SC_RUN_TEST(test_tone_neutral);
    SC_RUN_TEST(test_tone_casual);
    SC_RUN_TEST(test_tone_technical);
    SC_RUN_TEST(test_tone_formal);
    SC_RUN_TEST(test_tone_hint_string);
    SC_RUN_TEST(test_tone_null_input);

    SC_RUN_TEST(test_pref_is_correction_positive);
    SC_RUN_TEST(test_pref_is_correction_negative);
    SC_RUN_TEST(test_pref_extract);
    SC_RUN_TEST(test_pref_extract_null);

    SC_RUN_TEST(test_reflection_empty_response);
    SC_RUN_TEST(test_reflection_short_response);
    SC_RUN_TEST(test_reflection_good_response);
    SC_RUN_TEST(test_reflection_refusal_response);
    SC_RUN_TEST(test_reflection_build_critique);
    SC_RUN_TEST(test_reflection_result_free);

    SC_RUN_TEST(test_episodic_summarize_basic);
    SC_RUN_TEST(test_episodic_summarize_null);
    SC_RUN_TEST(test_episodic_load_null_out);
    SC_RUN_TEST(test_episodic_load_null_alloc);
    SC_RUN_TEST(test_episodic_store_null_memory);
    SC_RUN_TEST(test_episodic_store_null_summary);
    SC_RUN_TEST(test_episodic_summarize_null_messages);
    SC_RUN_TEST(test_episodic_summarize_zero_count);
#ifdef SC_ENABLE_SQLITE
    SC_RUN_TEST(test_episodic_store_load_sqlite);
#endif

    SC_RUN_TEST(test_awareness_init_deinit);
    SC_RUN_TEST(test_awareness_tracks_messages);
    SC_RUN_TEST(test_awareness_tracks_errors);
    SC_RUN_TEST(test_awareness_context_empty);
    SC_RUN_TEST(test_awareness_context_with_data);

    SC_RUN_TEST(test_prompt_with_persona);
    SC_RUN_TEST(test_prompt_with_tone_and_prefs);

    SC_RUN_TEST(test_compaction_llm_fallback);

    /* Upgrade 1: Awareness prompt injection */
    SC_RUN_TEST(test_awareness_prompt_injection);
    SC_RUN_TEST(test_awareness_prompt_null_skipped);

    /* Upgrade 2: Agent cron jobs */
    SC_RUN_TEST(test_cron_add_agent_job);
    SC_RUN_TEST(test_cron_add_agent_job_null_prompt);
    SC_RUN_TEST(test_cron_shell_job_type_default);

    /* Upgrade 3: Reflection retry */
    SC_RUN_TEST(test_reflection_needs_retry_on_empty);
    SC_RUN_TEST(test_reflection_good_after_long_response);
    SC_RUN_TEST(test_reflection_critique_prompt_format);

    /* Upgrade 4: Plan generation */
    SC_RUN_TEST(test_planner_generate_stub);
    SC_RUN_TEST(test_planner_generate_null_args);

    /* Upgrade 5: Outcome tracking */
    SC_RUN_TEST(test_outcome_tracker_init);
    SC_RUN_TEST(test_outcome_record_tool_success);
    SC_RUN_TEST(test_outcome_record_tool_failure);
    SC_RUN_TEST(test_outcome_record_correction);
    SC_RUN_TEST(test_outcome_record_positive);
    SC_RUN_TEST(test_outcome_get_recent);
    SC_RUN_TEST(test_outcome_build_summary);
    SC_RUN_TEST(test_outcome_detect_repeated_failure);
    SC_RUN_TEST(test_outcome_circular_buffer);
    SC_RUN_TEST(test_outcome_null_safety);
    SC_RUN_TEST(test_outcome_tracker_init_null);
    SC_RUN_TEST(test_outcome_get_recent_null_count);
    SC_RUN_TEST(test_outcome_build_summary_null_alloc);
    SC_RUN_TEST(test_outcome_build_summary_null_tracker);
    SC_RUN_TEST(test_outcome_detect_repeated_failure_null_tool_name);
    SC_RUN_TEST(test_outcome_detect_repeated_failure_zero_threshold);
}
