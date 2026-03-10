#include "human/agent/awareness.h"
#include "human/agent/compaction.h"
#include "human/agent/episodic.h"
#include "human/agent/outcomes.h"
#include "human/agent/planner.h"
#include "human/agent/preferences.h"
#include "human/agent/prompt.h"
#include "human/agent/reflection.h"
#include "human/bus.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/cron.h"
#include "human/memory.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

/* ── Tone detection ─────────────────────────────────────────────────────── */

static void test_tone_neutral(void) {
    const char *msgs[] = {"Hello, can you help me?"};
    size_t lens[] = {strlen(msgs[0])};
    hu_tone_t t = hu_detect_tone(msgs, lens, 1);
    HU_ASSERT_EQ(t, HU_TONE_NEUTRAL);
}

static void test_tone_casual(void) {
    const char *msgs[] = {"lol hey!", "omg that's cool!", "haha nice!"};
    size_t lens[] = {strlen(msgs[0]), strlen(msgs[1]), strlen(msgs[2])};
    hu_tone_t t = hu_detect_tone(msgs, lens, 3);
    HU_ASSERT_EQ(t, HU_TONE_CASUAL);
}

static void test_tone_technical(void) {
    const char *msgs[] = {"I'm getting a stack trace error in src/main.c",
                          "The config debug output shows `/etc/app.conf`",
                          "Here is the error ```segfault```"};
    size_t lens[] = {strlen(msgs[0]), strlen(msgs[1]), strlen(msgs[2])};
    hu_tone_t t = hu_detect_tone(msgs, lens, 3);
    HU_ASSERT_EQ(t, HU_TONE_TECHNICAL);
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
    hu_tone_t t = hu_detect_tone(msgs, lens, 3);
    HU_ASSERT_EQ(t, HU_TONE_FORMAL);
}

static void test_tone_hint_string(void) {
    size_t len = 0;
    const char *s = hu_tone_hint_string(HU_TONE_CASUAL, &len);
    HU_ASSERT_NOT_NULL(s);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(s, "casually") != NULL);

    s = hu_tone_hint_string(HU_TONE_NEUTRAL, &len);
    HU_ASSERT_NULL(s);
    HU_ASSERT_EQ(len, 0);
}

static void test_tone_null_input(void) {
    hu_tone_t t = hu_detect_tone(NULL, NULL, 0);
    HU_ASSERT_EQ(t, HU_TONE_NEUTRAL);
}

/* ── Preferences ────────────────────────────────────────────────────────── */

static void test_pref_is_correction_positive(void) {
    HU_ASSERT_TRUE(hu_preferences_is_correction("No, I want JSON output", 22));
    HU_ASSERT_TRUE(hu_preferences_is_correction("actually, use tabs", 18));
    HU_ASSERT_TRUE(hu_preferences_is_correction("I prefer dark mode", 18));
    HU_ASSERT_TRUE(hu_preferences_is_correction("Don't use emojis", 16));
    HU_ASSERT_TRUE(hu_preferences_is_correction("Stop using semicolons", 21));
    HU_ASSERT_TRUE(hu_preferences_is_correction("Never add comments", 18));
    HU_ASSERT_TRUE(hu_preferences_is_correction("Always use const", 16));
    HU_ASSERT_TRUE(hu_preferences_is_correction("Instead, return an error", 24));
}

static void test_pref_is_correction_negative(void) {
    HU_ASSERT_FALSE(hu_preferences_is_correction("What is rust?", 13));
    HU_ASSERT_FALSE(hu_preferences_is_correction("Please explain this", 19));
    HU_ASSERT_FALSE(hu_preferences_is_correction("How do I compile?", 17));
    HU_ASSERT_FALSE(hu_preferences_is_correction(NULL, 0));
    HU_ASSERT_FALSE(hu_preferences_is_correction("ab", 2));
}

static void test_pref_extract(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *pref = hu_preferences_extract(&alloc, "  Always use tabs  ", 19, &len);
    HU_ASSERT_NOT_NULL(pref);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(pref, "Always") != NULL);
    alloc.free(alloc.ctx, pref, len + 1);
}

static void test_pref_extract_null(void) {
    size_t len = 0;
    char *pref = hu_preferences_extract(NULL, "test", 4, &len);
    HU_ASSERT_NULL(pref);
}

/* ── Reflection ─────────────────────────────────────────────────────────── */

static void test_reflection_empty_response(void) {
    hu_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 0, .max_retries = 1};
    hu_reflection_quality_t q = hu_reflection_evaluate("hello?", 6, "", 0, &cfg);
    HU_ASSERT_EQ(q, HU_QUALITY_NEEDS_RETRY);
}

static void test_reflection_short_response(void) {
    hu_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 0, .max_retries = 1};
    hu_reflection_quality_t q = hu_reflection_evaluate("hello?", 6, "Hi", 2, &cfg);
    HU_ASSERT_EQ(q, HU_QUALITY_NEEDS_RETRY);
}

static void test_reflection_good_response(void) {
    hu_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 0, .max_retries = 1};
    const char *resp = "Here is a detailed answer to your question about the configuration "
                       "settings and how they interact with the runtime environment.";
    hu_reflection_quality_t q =
        hu_reflection_evaluate("How does config work?", 21, resp, strlen(resp), &cfg);
    HU_ASSERT_EQ(q, HU_QUALITY_GOOD);
}

static void test_reflection_refusal_response(void) {
    hu_reflection_config_t cfg = {.enabled = true};
    const char *resp = "I cannot help with that request as an AI.";
    hu_reflection_quality_t q =
        hu_reflection_evaluate("do something", 12, resp, strlen(resp), &cfg);
    HU_ASSERT_EQ(q, HU_QUALITY_ACCEPTABLE);
}

static void test_reflection_build_critique(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *prompt = NULL;
    size_t prompt_len = 0;
    hu_error_t err = hu_reflection_build_critique_prompt(&alloc, "How?", 4, "Like this.", 10,
                                                         &prompt, &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(strstr(prompt, "Evaluate") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "How?") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "Like this.") != NULL);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
}

static void test_reflection_result_free(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_reflection_result_t r = {.quality = HU_QUALITY_GOOD, .feedback = NULL, .feedback_len = 0};
    hu_reflection_result_free(&alloc, &r);
    hu_reflection_result_free(NULL, NULL);
}

/* ── Episodic memory ────────────────────────────────────────────────────── */

static void test_episodic_summarize_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"How do I configure the logger?", "Use hu_log_init with level param."};
    size_t lens[] = {strlen(msgs[0]), strlen(msgs[1])};
    size_t out_len = 0;
    char *summary = hu_episodic_summarize_session(&alloc, msgs, lens, 2, &out_len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(summary, "Session topic:") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "configure") != NULL);
    alloc.free(alloc.ctx, summary, out_len + 1);
}

static void test_episodic_summarize_null(void) {
    char *summary = hu_episodic_summarize_session(NULL, NULL, NULL, 0, NULL);
    HU_ASSERT_NULL(summary);
}

static void test_episodic_load_null_out(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    size_t out_len = 0;
    hu_error_t err = hu_episodic_load(&mem, &alloc, NULL, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

static void test_episodic_load_null_alloc(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_episodic_load(&mem, NULL, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

static void test_episodic_store_null_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_episodic_store(NULL, &alloc, "s", 1, "summary", 7);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_episodic_store_null_summary(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_none_memory_create(&alloc);
    hu_error_t err = hu_episodic_store(&mem, &alloc, "s", 1, NULL, 0);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    mem.vtable->deinit(mem.ctx);
}

static void test_episodic_summarize_null_messages(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t lens[] = {5};
    size_t out_len = 0;
    char *summary = hu_episodic_summarize_session(&alloc, NULL, lens, 1, &out_len);
    HU_ASSERT_NULL(summary);
}

static void test_episodic_summarize_zero_count(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *msgs[] = {"hello"};
    size_t lens[] = {5};
    size_t out_len = 0;
    char *summary = hu_episodic_summarize_session(&alloc, msgs, lens, 0, &out_len);
    HU_ASSERT_NULL(summary);
}

#ifdef HU_ENABLE_SQLITE
static void test_episodic_store_load_sqlite(void) {
    hu_allocator_t alloc = hu_system_allocator();

    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    HU_ASSERT_NOT_NULL(mem.vtable);
    HU_ASSERT_NOT_NULL(mem.ctx);

    hu_error_t err =
        hu_episodic_store(&mem, &alloc, "session_abc", 11, "User asked about config parsing", 31);
    HU_ASSERT_EQ(err, HU_OK);

    char *out = NULL;
    size_t out_len = 0;
    err = hu_episodic_load(&mem, &alloc, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "config parsing") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
    mem.vtable->deinit(mem.ctx);
}
#endif

/* ── Awareness ──────────────────────────────────────────────────────────── */

static void test_awareness_init_deinit(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_error_t err = hu_awareness_init(&aw, &bus);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(aw.state.messages_received, 0);
    hu_awareness_deinit(&aw);
}

static void test_awareness_tracks_messages(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    hu_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = HU_BUS_MESSAGE_RECEIVED;
    strncpy(ev.channel, "cli", HU_BUS_CHANNEL_LEN);
    hu_bus_publish(&bus, &ev);

    HU_ASSERT_EQ(aw.state.messages_received, 1);
    HU_ASSERT_EQ(aw.state.active_channel_count, 1);

    ev.type = HU_BUS_TOOL_CALL;
    hu_bus_publish(&bus, &ev);
    HU_ASSERT_EQ(aw.state.tool_calls, 1);

    hu_awareness_deinit(&aw);
}

static void test_awareness_tracks_errors(void) {
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    hu_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = HU_BUS_ERROR;
    strncpy(ev.message, "provider timeout", HU_BUS_MSG_LEN);
    hu_bus_publish(&bus, &ev);

    HU_ASSERT_EQ(aw.state.total_errors, 1);
    HU_ASSERT_TRUE(strstr(aw.state.recent_errors[0].text, "provider timeout") != NULL);

    hu_awareness_deinit(&aw);
}

static void test_awareness_context_empty(void) {
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

static void test_awareness_context_with_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bus_t bus;
    hu_bus_init(&bus);
    hu_awareness_t aw;
    hu_awareness_init(&aw, &bus);

    hu_bus_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = HU_BUS_MESSAGE_RECEIVED;
    strncpy(ev.channel, "telegram", HU_BUS_CHANNEL_LEN);
    hu_bus_publish(&bus, &ev);

    ev.type = HU_BUS_ERROR;
    strncpy(ev.message, "test error", HU_BUS_MSG_LEN);
    hu_bus_publish(&bus, &ev);

    size_t len = 0;
    char *ctx = hu_awareness_context(&aw, &alloc, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "Situational Awareness") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "telegram") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "test error") != NULL);

    alloc.free(alloc.ctx, ctx, len + 1);
    hu_awareness_deinit(&aw);
}

/* ── Prompt with new fields ─────────────────────────────────────────────── */

static void test_prompt_with_persona(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg = {
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
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Jarvis") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Human") == NULL);
    HU_ASSERT_TRUE(strstr(out, "Reasoning") != NULL);
    HU_ASSERT_TRUE(strstr(out, "step by step") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

static void test_prompt_with_tone_and_prefs(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg = {
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
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Tone") != NULL);
    HU_ASSERT_TRUE(strstr(out, "casual") != NULL);
    HU_ASSERT_TRUE(strstr(out, "User Preferences") != NULL);
    HU_ASSERT_TRUE(strstr(out, "snake_case") != NULL);

    alloc.free(alloc.ctx, out, out_len + 1);
}

/* ── LLM compaction (provider=NULL fallback) ────────────────────────────── */

static void test_compaction_llm_fallback(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_compaction_config_t cfg;
    hu_compaction_config_default(&cfg);
    cfg.keep_recent = 2;
    cfg.max_history_messages = 3;

    hu_owned_message_t msgs[5];
    memset(msgs, 0, sizeof(msgs));

    msgs[0].role = HU_ROLE_SYSTEM;
    msgs[0].content = hu_strndup(&alloc, "System prompt", 13);
    msgs[0].content_len = 13;

    for (int i = 1; i < 5; i++) {
        char buf[32];
        int n = snprintf(buf, sizeof(buf), "Message number %d", i);
        msgs[i].role = (i % 2 == 1) ? HU_ROLE_USER : HU_ROLE_ASSISTANT;
        msgs[i].content = hu_strndup(&alloc, buf, (size_t)n);
        msgs[i].content_len = (size_t)n;
    }

    size_t count = 5;
    size_t cap = 5;

    hu_error_t err = hu_compact_history(&alloc, msgs, &count, &cap, &cfg);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(count < 5);

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
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.awareness_context = "## Situational Awareness\n- Session stats: 5 msgs received\n";
    cfg.awareness_context_len = strlen(cfg.awareness_context);

    char *prompt = NULL;
    size_t prompt_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &prompt, &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(prompt);
    HU_ASSERT_TRUE(strstr(prompt, "Situational Awareness") != NULL);
    HU_ASSERT_TRUE(strstr(prompt, "5 msgs received") != NULL);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
}

static void test_awareness_prompt_null_skipped(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_prompt_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    char *prompt = NULL;
    size_t prompt_len = 0;
    hu_error_t err = hu_prompt_build_system(&alloc, &cfg, &prompt, &prompt_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(prompt, "Situational Awareness") == NULL);
    alloc.free(alloc.ctx, prompt, prompt_len + 1);
}

/* ── Upgrade 2: Agent cron jobs ─────────────────────────────────────── */

static void test_cron_add_agent_job(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *sched = hu_cron_create(&alloc, 64, true);
    HU_ASSERT_NOT_NULL(sched);

    uint64_t id = 0;
    hu_error_t err = hu_cron_add_agent_job(sched, &alloc, "0 8 * * *",
                                           "Check my calendar and send a daily brief", "slack",
                                           "daily-brief", &id);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(id > 0);

    const hu_cron_job_t *job = hu_cron_get_job(sched, id);
    HU_ASSERT_NOT_NULL(job);
    HU_ASSERT_EQ(job->type, HU_CRON_JOB_AGENT);
    HU_ASSERT_NOT_NULL(job->channel);
    HU_ASSERT_TRUE(strcmp(job->channel, "slack") == 0);
    HU_ASSERT_TRUE(strcmp(job->command, "Check my calendar and send a daily brief") == 0);

    hu_cron_destroy(sched, &alloc);
}

static void test_cron_add_agent_job_null_prompt(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *sched = hu_cron_create(&alloc, 64, true);
    uint64_t id = 0;
    hu_error_t err = hu_cron_add_agent_job(sched, &alloc, "* * * * *", NULL, NULL, NULL, &id);
    HU_ASSERT_NEQ(err, HU_OK);
    hu_cron_destroy(sched, &alloc);
}

static void test_cron_shell_job_type_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_cron_scheduler_t *sched = hu_cron_create(&alloc, 64, true);
    uint64_t id = 0;
    hu_error_t err = hu_cron_add_job(sched, &alloc, "* * * * *", "echo hello", NULL, &id);
    HU_ASSERT_EQ(err, HU_OK);
    const hu_cron_job_t *job = hu_cron_get_job(sched, id);
    HU_ASSERT_NOT_NULL(job);
    HU_ASSERT_EQ(job->type, HU_CRON_JOB_SHELL);
    hu_cron_destroy(sched, &alloc);
}

/* ── Upgrade 3: Reflection retry ────────────────────────────────────── */

static void test_reflection_needs_retry_on_empty(void) {
    hu_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 0, .max_retries = 2};
    hu_reflection_quality_t q = hu_reflection_evaluate("hello?", 6, "", 0, &cfg);
    HU_ASSERT_EQ(q, HU_QUALITY_NEEDS_RETRY);
}

static void test_reflection_good_after_long_response(void) {
    hu_reflection_config_t cfg = {.enabled = true, .min_response_tokens = 0, .max_retries = 2};
    const char *resp = "This is a comprehensive answer that thoroughly addresses the question "
                       "about configuration settings and runtime behavior.";
    hu_reflection_quality_t q =
        hu_reflection_evaluate("How does config?", 16, resp, strlen(resp), &cfg);
    HU_ASSERT_EQ(q, HU_QUALITY_GOOD);
}

static void test_reflection_critique_prompt_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *critique = NULL;
    size_t len = 0;
    hu_error_t err =
        hu_reflection_build_critique_prompt(&alloc, "what is 2+2?", 12, "idk", 3, &critique, &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(critique);
    HU_ASSERT_TRUE(strstr(critique, "what is 2+2?") != NULL);
    HU_ASSERT_TRUE(strstr(critique, "idk") != NULL);
    alloc.free(alloc.ctx, critique, len + 1);
}

/* ── Upgrade 4: Plan generation ─────────────────────────────────────── */

static void test_planner_generate_stub(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_t *plan = NULL;
    const char *tools[] = {"shell", "file_read", "web_search"};

    /* In test mode, hu_planner_generate returns a stub plan */
    hu_provider_vtable_t dummy_vtable;
    memset(&dummy_vtable, 0, sizeof(dummy_vtable));
    hu_provider_t dummy_provider = {.vtable = &dummy_vtable, .ctx = NULL};

    hu_error_t err =
        hu_planner_generate(&alloc, &dummy_provider, "gpt-4o", 6,
                            "Find all TODO comments in the codebase", 39, tools, 3, &plan);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(plan);
    HU_ASSERT_TRUE(plan->steps_count > 0);
    HU_ASSERT_NOT_NULL(plan->steps[0].tool_name);
    hu_plan_free(&alloc, plan);
}

static void test_planner_generate_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_t *plan = NULL;
    hu_error_t err = hu_planner_generate(NULL, NULL, NULL, 0, NULL, 0, NULL, 0, &plan);
    HU_ASSERT_NEQ(err, HU_OK);
    err = hu_planner_generate(&alloc, NULL, NULL, 0, "test", 4, NULL, 0, &plan);
    HU_ASSERT_NEQ(err, HU_OK);
}

static void test_planner_replan_basic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_t *plan = NULL;
    const char *tools[] = {"shell", "file_read", "web_search"};

    hu_provider_vtable_t dummy_vtable;
    memset(&dummy_vtable, 0, sizeof(dummy_vtable));
    hu_provider_t dummy_provider = {.vtable = &dummy_vtable, .ctx = NULL};

    hu_error_t err = hu_planner_replan(&alloc, &dummy_provider, "gpt-4o", 6,
                                       "Find TODOs in codebase", 23, "  [1] grep: done\n", 18,
                                       "shell: command not found", 24, tools, 3, &plan);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(plan);
    HU_ASSERT_TRUE(plan->steps_count > 0);
    HU_ASSERT_NOT_NULL(plan->steps[0].tool_name);
    HU_ASSERT_TRUE(strcmp(plan->steps[0].tool_name, "shell") == 0);
    hu_plan_free(&alloc, plan);
}

static void test_planner_replan_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_t *plan = NULL;
    const char *tools[] = {"shell"};

    hu_provider_vtable_t dummy_vtable;
    memset(&dummy_vtable, 0, sizeof(dummy_vtable));
    hu_provider_t dummy_provider = {.vtable = &dummy_vtable, .ctx = NULL};

    hu_error_t err = hu_planner_replan(NULL, &dummy_provider, "gpt-4o", 6, "goal", 4, "progress", 8,
                                       "fail", 4, tools, 1, &plan);
    HU_ASSERT_NEQ(err, HU_OK);

    err = hu_planner_replan(&alloc, NULL, "gpt-4o", 6, "goal", 4, "progress", 8, "fail", 4, tools,
                            1, &plan);
    HU_ASSERT_NEQ(err, HU_OK);

    err = hu_planner_replan(&alloc, &dummy_provider, "gpt-4o", 6, NULL, 0, "progress", 8, "fail", 4,
                            tools, 1, &plan);
    HU_ASSERT_NEQ(err, HU_OK);
}

/* ── Upgrade 5: Outcome tracking ─────────────────────────────────────── */

static void test_outcome_tracker_init(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, true);
    HU_ASSERT_EQ(tracker.total, 0);
    HU_ASSERT_EQ(tracker.tool_successes, 0);
    HU_ASSERT_EQ(tracker.tool_failures, 0);
    HU_ASSERT_EQ(tracker.corrections, 0);
    HU_ASSERT_EQ(tracker.positives, 0);
    HU_ASSERT_TRUE(tracker.auto_apply_feedback);
}

static void test_outcome_record_tool_success(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_tool(&tracker, "shell", true, "executed echo hello");
    HU_ASSERT_EQ(tracker.total, 1);
    HU_ASSERT_EQ(tracker.tool_successes, 1);
    HU_ASSERT_EQ(tracker.tool_failures, 0);
}

static void test_outcome_record_tool_failure(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_tool(&tracker, "shell", false, "command not found");
    HU_ASSERT_EQ(tracker.total, 1);
    HU_ASSERT_EQ(tracker.tool_successes, 0);
    HU_ASSERT_EQ(tracker.tool_failures, 1);
}

static void test_outcome_record_correction(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_correction(&tracker, "I said X", "no, actually Y");
    HU_ASSERT_EQ(tracker.total, 1);
    HU_ASSERT_EQ(tracker.corrections, 1);
}

static void test_outcome_record_positive(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_positive(&tracker, "thanks, that's great");
    HU_ASSERT_EQ(tracker.total, 1);
    HU_ASSERT_EQ(tracker.positives, 1);
}

static void test_outcome_get_recent(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_tool(&tracker, "shell", true, "ok");
    hu_outcome_record_tool(&tracker, "web_search", false, "timeout");
    size_t count = 0;
    const hu_outcome_entry_t *entries = hu_outcome_get_recent(&tracker, &count);
    HU_ASSERT_NOT_NULL(entries);
    HU_ASSERT_EQ(count, 2);
}

static void test_outcome_build_summary(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);

    char *summary = hu_outcome_build_summary(&tracker, &alloc, NULL);
    HU_ASSERT_NULL(summary); /* empty tracker returns NULL */

    hu_outcome_record_tool(&tracker, "shell", true, "ok");
    hu_outcome_record_tool(&tracker, "web_search", false, "fail");
    hu_outcome_record_correction(&tracker, NULL, "actually do X");

    size_t len = 0;
    summary = hu_outcome_build_summary(&tracker, &alloc, &len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(summary, "1 succeeded") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "1 failed") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "corrections: 1") != NULL);
    alloc.free(alloc.ctx, summary, len + 1);
}

static void test_outcome_detect_repeated_failure(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    HU_ASSERT_FALSE(hu_outcome_detect_repeated_failure(&tracker, "shell", 3));
    hu_outcome_record_tool(&tracker, "shell", false, "err1");
    hu_outcome_record_tool(&tracker, "shell", false, "err2");
    HU_ASSERT_FALSE(hu_outcome_detect_repeated_failure(&tracker, "shell", 3));
    hu_outcome_record_tool(&tracker, "shell", false, "err3");
    HU_ASSERT_TRUE(hu_outcome_detect_repeated_failure(&tracker, "shell", 3));
}

static void test_outcome_circular_buffer(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    for (size_t i = 0; i < HU_OUTCOME_MAX_RECENT + 10; i++)
        hu_outcome_record_tool(&tracker, "shell", true, "ok");
    HU_ASSERT_EQ(tracker.total, HU_OUTCOME_MAX_RECENT + 10);
    size_t count = 0;
    (void)hu_outcome_get_recent(&tracker, &count);
    HU_ASSERT_EQ(count, HU_OUTCOME_MAX_RECENT);
}

static void test_outcome_null_safety(void) {
    hu_outcome_record_tool(NULL, "shell", true, "ok");
    hu_outcome_record_correction(NULL, NULL, NULL);
    hu_outcome_record_positive(NULL, NULL);
    HU_ASSERT_FALSE(hu_outcome_detect_repeated_failure(NULL, "shell", 1));
    size_t count = 0;
    HU_ASSERT_NULL(hu_outcome_get_recent(NULL, &count));
}

static void test_outcome_tracker_init_null(void) {
    hu_outcome_tracker_init(NULL, true);
    /* no-op, should not crash */
}

static void test_outcome_get_recent_null_count(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_tool(&tracker, "shell", true, "ok");
    HU_ASSERT_NULL(hu_outcome_get_recent(&tracker, NULL));
}

static void test_outcome_build_summary_null_alloc(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_tool(&tracker, "shell", true, "ok");
    HU_ASSERT_NULL(hu_outcome_build_summary(&tracker, NULL, NULL));
}

static void test_outcome_build_summary_null_tracker(void) {
    hu_allocator_t alloc = hu_system_allocator();
    HU_ASSERT_NULL(hu_outcome_build_summary(NULL, &alloc, NULL));
}

static void test_outcome_detect_repeated_failure_null_tool_name(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    HU_ASSERT_FALSE(hu_outcome_detect_repeated_failure(&tracker, NULL, 1));
}

static void test_outcome_detect_repeated_failure_zero_threshold(void) {
    hu_outcome_tracker_t tracker;
    hu_outcome_tracker_init(&tracker, false);
    hu_outcome_record_tool(&tracker, "shell", false, "err");
    HU_ASSERT_FALSE(hu_outcome_detect_repeated_failure(&tracker, "shell", 0));
}

/* ── Run all ────────────────────────────────────────────────────────────── */

void run_intelligence_tests(void) {
    HU_TEST_SUITE("Intelligence Enhancement");

    HU_RUN_TEST(test_tone_neutral);
    HU_RUN_TEST(test_tone_casual);
    HU_RUN_TEST(test_tone_technical);
    HU_RUN_TEST(test_tone_formal);
    HU_RUN_TEST(test_tone_hint_string);
    HU_RUN_TEST(test_tone_null_input);

    HU_RUN_TEST(test_pref_is_correction_positive);
    HU_RUN_TEST(test_pref_is_correction_negative);
    HU_RUN_TEST(test_pref_extract);
    HU_RUN_TEST(test_pref_extract_null);

    HU_RUN_TEST(test_reflection_empty_response);
    HU_RUN_TEST(test_reflection_short_response);
    HU_RUN_TEST(test_reflection_good_response);
    HU_RUN_TEST(test_reflection_refusal_response);
    HU_RUN_TEST(test_reflection_build_critique);
    HU_RUN_TEST(test_reflection_result_free);

    HU_RUN_TEST(test_episodic_summarize_basic);
    HU_RUN_TEST(test_episodic_summarize_null);
    HU_RUN_TEST(test_episodic_load_null_out);
    HU_RUN_TEST(test_episodic_load_null_alloc);
    HU_RUN_TEST(test_episodic_store_null_memory);
    HU_RUN_TEST(test_episodic_store_null_summary);
    HU_RUN_TEST(test_episodic_summarize_null_messages);
    HU_RUN_TEST(test_episodic_summarize_zero_count);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_episodic_store_load_sqlite);
#endif

    HU_RUN_TEST(test_awareness_init_deinit);
    HU_RUN_TEST(test_awareness_tracks_messages);
    HU_RUN_TEST(test_awareness_tracks_errors);
    HU_RUN_TEST(test_awareness_context_empty);
    HU_RUN_TEST(test_awareness_context_with_data);

    HU_RUN_TEST(test_prompt_with_persona);
    HU_RUN_TEST(test_prompt_with_tone_and_prefs);

    HU_RUN_TEST(test_compaction_llm_fallback);

    /* Upgrade 1: Awareness prompt injection */
    HU_RUN_TEST(test_awareness_prompt_injection);
    HU_RUN_TEST(test_awareness_prompt_null_skipped);

    /* Upgrade 2: Agent cron jobs */
    HU_RUN_TEST(test_cron_add_agent_job);
    HU_RUN_TEST(test_cron_add_agent_job_null_prompt);
    HU_RUN_TEST(test_cron_shell_job_type_default);

    /* Upgrade 3: Reflection retry */
    HU_RUN_TEST(test_reflection_needs_retry_on_empty);
    HU_RUN_TEST(test_reflection_good_after_long_response);
    HU_RUN_TEST(test_reflection_critique_prompt_format);

    /* Upgrade 4: Plan generation */
    HU_RUN_TEST(test_planner_generate_stub);
    HU_RUN_TEST(test_planner_generate_null_args);
    HU_RUN_TEST(test_planner_replan_basic);
    HU_RUN_TEST(test_planner_replan_null_args);

    /* Upgrade 5: Outcome tracking */
    HU_RUN_TEST(test_outcome_tracker_init);
    HU_RUN_TEST(test_outcome_record_tool_success);
    HU_RUN_TEST(test_outcome_record_tool_failure);
    HU_RUN_TEST(test_outcome_record_correction);
    HU_RUN_TEST(test_outcome_record_positive);
    HU_RUN_TEST(test_outcome_get_recent);
    HU_RUN_TEST(test_outcome_build_summary);
    HU_RUN_TEST(test_outcome_detect_repeated_failure);
    HU_RUN_TEST(test_outcome_circular_buffer);
    HU_RUN_TEST(test_outcome_null_safety);
    HU_RUN_TEST(test_outcome_tracker_init_null);
    HU_RUN_TEST(test_outcome_get_recent_null_count);
    HU_RUN_TEST(test_outcome_build_summary_null_alloc);
    HU_RUN_TEST(test_outcome_build_summary_null_tracker);
    HU_RUN_TEST(test_outcome_detect_repeated_failure_null_tool_name);
    HU_RUN_TEST(test_outcome_detect_repeated_failure_zero_threshold);
}
