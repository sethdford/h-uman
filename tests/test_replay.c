#include "seaclaw/core/allocator.h"
#include "seaclaw/memory.h"
#include "seaclaw/memory/engines.h"
#include "seaclaw/persona/auto_tune.h"
#include "seaclaw/persona/replay.h"
#include "test_framework.h"
#include <string.h>

static sc_channel_history_entry_t make_entry(bool from_me, const char *text, const char *ts) {
    sc_channel_history_entry_t e;
    memset(&e, 0, sizeof(e));
    e.from_me = from_me;
    size_t tl = strlen(text);
    if (tl >= sizeof(e.text))
        tl = sizeof(e.text) - 1;
    memcpy(e.text, text, tl);
    e.text[tl] = '\0';
    size_t tsl = strlen(ts);
    if (tsl >= sizeof(e.timestamp))
        tsl = sizeof(e.timestamp) - 1;
    memcpy(e.timestamp, ts, tsl);
    e.timestamp[tsl] = '\0';
    return e;
}

static void replay_detects_positive_signal(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_history_entry_t entries[2] = {
        make_entry(true, "that's so funny", "12:00"),
        make_entry(false, "haha exactly!", "12:01"),
    };
    sc_replay_result_t result = {0};
    sc_error_t err = sc_replay_analyze(&alloc, entries, 2, 300, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(result.insight_count >= 1);
    bool found_positive = false;
    for (size_t i = 0; i < result.insight_count; i++) {
        if (result.insights[i].score_delta > 0) {
            found_positive = true;
            SC_ASSERT_TRUE(result.insights[i].observation != NULL);
            SC_ASSERT_TRUE(strstr(result.insights[i].observation, "engagement") != NULL ||
                           strstr(result.insights[i].observation, "enthusiastic") != NULL);
            break;
        }
    }
    SC_ASSERT_TRUE(found_positive);
    sc_replay_result_deinit(&result, &alloc);
}

static void replay_detects_robotic_tell(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_history_entry_t entries[2] = {
        make_entry(false, "I've been really stressed about work", "12:00"),
        make_entry(true, "I understand how you feel. That must be difficult.", "12:01"),
    };
    sc_replay_result_t result = {0};
    sc_error_t err = sc_replay_analyze(&alloc, entries, 2, 300, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(result.insight_count >= 1);
    bool found_negative = false;
    for (size_t i = 0; i < result.insight_count; i++) {
        if (result.insights[i].score_delta < 0) {
            found_negative = true;
            SC_ASSERT_TRUE(result.insights[i].observation != NULL);
            SC_ASSERT_TRUE(strstr(result.insights[i].observation, "specificity") != NULL ||
                           strstr(result.insights[i].observation, "follow-up") != NULL ||
                           strstr(result.insights[i].observation, "robotic") != NULL);
            break;
        }
    }
    SC_ASSERT_TRUE(found_negative);
    sc_replay_result_deinit(&result, &alloc);
}

static void replay_empty_history(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_replay_result_t result = {0};
    sc_error_t err = sc_replay_analyze(&alloc, NULL, 0, 300, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_EQ(result.insight_count, 0u);
    SC_ASSERT_EQ(result.overall_score, 0);
}

static void replay_null_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_history_entry_t entries[1] = {make_entry(true, "hi", "12:00")};
    sc_replay_result_t result = {0};

    sc_error_t err = sc_replay_analyze(NULL, entries, 1, 300, &result);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);

    err = sc_replay_analyze(&alloc, entries, 1, 300, NULL);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);

    err = sc_replay_analyze(&alloc, NULL, 1, 300, &result);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void replay_build_context_with_insights(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_channel_history_entry_t entries[2] = {
        make_entry(true, "that's awesome", "12:00"),
        make_entry(false, "yes!!", "12:01"),
    };
    sc_replay_result_t result = {0};
    sc_error_t err = sc_replay_analyze(&alloc, entries, 2, 300, &result);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_TRUE(result.insight_count >= 1);

    size_t len = 0;
    char *ctx = sc_replay_build_context(&alloc, &result, &len);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(strstr(ctx, "Session Replay") != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);
    sc_replay_result_deinit(&result, &alloc);
}

static void replay_build_context_empty_returns_null(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_replay_result_t result = {0};
    result.insight_count = 0;
    size_t len = 0;
    char *ctx = sc_replay_build_context(&alloc, &result, &len);
    SC_ASSERT_NULL(ctx);
    SC_ASSERT_EQ(len, 0u);
}

static void auto_tune_no_memory_returns_error(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *summary = NULL;
    size_t summary_len = 0;
    sc_error_t err = sc_replay_auto_tune(&alloc, NULL, "contact_a", 9, &summary, &summary_len);
    SC_ASSERT_EQ(err, SC_ERR_INVALID_ARGUMENT);
}

static void auto_tune_null_args(void) {
    sc_allocator_t alloc = sc_system_allocator();
    char *summary = NULL;
    size_t summary_len = 0;
    SC_ASSERT_EQ(sc_replay_auto_tune(NULL, NULL, NULL, 0, &summary, &summary_len),
                 SC_ERR_INVALID_ARGUMENT);
    SC_ASSERT_EQ(sc_replay_auto_tune(&alloc, NULL, NULL, 0, NULL, &summary_len),
                 SC_ERR_INVALID_ARGUMENT);
}

static void auto_tune_empty_memory(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    SC_ASSERT_NOT_NULL(mem.ctx);

    char *summary = NULL;
    size_t summary_len = 0;
    sc_error_t err = sc_replay_auto_tune(&alloc, &mem, "contact_a", 9, &summary, &summary_len);
    SC_ASSERT_EQ(err, SC_OK);

    if (summary)
        alloc.free(alloc.ctx, summary, summary_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void auto_tune_with_stored_insights(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_memory_t mem = sc_memory_lru_create(&alloc, 100);
    SC_ASSERT_NOT_NULL(mem.ctx);

    static const char cat_name[] = "replay_insights";
    sc_memory_category_t cat = {
        .tag = SC_MEMORY_CATEGORY_CUSTOM,
        .data.custom = {.name = cat_name, .name_len = sizeof(cat_name) - 1},
    };

    const char *insight1 = "POSITIVE: enthusiastic reply after casual joke\nNEGATIVE: robotic tell in greeting";
    const char *insight2 = "POSITIVE: enthusiastic reply after music recommendation\nPOSITIVE: fast response to question";
    const char *insight3 = "NEGATIVE: robotic tell in opening\nNEGATIVE: formulaic response";

    mem.vtable->store(mem.ctx, "replay:1", 8, insight1, strlen(insight1), &cat, "contact_a", 9);
    mem.vtable->store(mem.ctx, "replay:2", 8, insight2, strlen(insight2), &cat, "contact_a", 9);
    mem.vtable->store(mem.ctx, "replay:3", 8, insight3, strlen(insight3), &cat, "contact_a", 9);

    char *summary = NULL;
    size_t summary_len = 0;
    sc_error_t err = sc_replay_auto_tune(&alloc, &mem, NULL, 0, &summary, &summary_len);
    SC_ASSERT_EQ(err, SC_OK);
    SC_ASSERT_NOT_NULL(summary);
    SC_ASSERT_TRUE(summary_len > 0);
    SC_ASSERT_TRUE(strstr(summary, "enthusiastic") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "robotic") != NULL);

    alloc.free(alloc.ctx, summary, summary_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void tune_build_context_formats(void) {
    sc_allocator_t alloc = sc_system_allocator();
    const char *summary = "WHAT'S WORKING: casual humor\nWHAT TO AVOID: robotic greetings";
    size_t out_len = 0;
    char *ctx = sc_replay_tune_build_context(&alloc, summary, strlen(summary), &out_len);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(out_len > 0);
    SC_ASSERT_TRUE(strstr(ctx, "Tone Calibration") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "casual humor") != NULL);
    alloc.free(alloc.ctx, ctx, out_len + 1);
}

static void tune_build_context_null(void) {
    sc_allocator_t alloc = sc_system_allocator();
    size_t out_len = 0;
    char *ctx = sc_replay_tune_build_context(&alloc, NULL, 0, &out_len);
    SC_ASSERT_NULL(ctx);
}

void run_replay_tests(void) {
    SC_TEST_SUITE("replay");
    SC_RUN_TEST(replay_detects_positive_signal);
    SC_RUN_TEST(replay_detects_robotic_tell);
    SC_RUN_TEST(replay_empty_history);
    SC_RUN_TEST(replay_null_args);
    SC_RUN_TEST(replay_build_context_with_insights);
    SC_RUN_TEST(replay_build_context_empty_returns_null);

    SC_TEST_SUITE("replay — auto-tune");
    SC_RUN_TEST(auto_tune_no_memory_returns_error);
    SC_RUN_TEST(auto_tune_null_args);
    SC_RUN_TEST(auto_tune_empty_memory);
    SC_RUN_TEST(auto_tune_with_stored_insights);
    SC_RUN_TEST(tune_build_context_formats);
    SC_RUN_TEST(tune_build_context_null);
}
