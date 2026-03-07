#include "seaclaw/agent/awareness.h"
#include "seaclaw/agent/compaction.h"
#include "seaclaw/agent/episodic.h"
#include "seaclaw/agent/preferences.h"
#include "seaclaw/agent/prompt.h"
#include "seaclaw/agent/reflection.h"
#include "seaclaw/bus.h"
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
}
