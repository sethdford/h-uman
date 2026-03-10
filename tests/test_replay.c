#include "human/core/allocator.h"
#include "human/memory.h"
#include "human/memory/engines.h"
#include "human/persona/auto_tune.h"
#include "human/persona/replay.h"
#include "test_framework.h"
#include <string.h>

static hu_channel_history_entry_t make_entry(bool from_me, const char *text, const char *ts) {
    hu_channel_history_entry_t e;
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
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[2] = {
        make_entry(true, "that's so funny", "12:00"),
        make_entry(false, "haha exactly!", "12:01"),
    };
    hu_replay_result_t result = {0};
    hu_error_t err = hu_replay_analyze(&alloc, entries, 2, 300, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.insight_count >= 1);
    bool found_positive = false;
    for (size_t i = 0; i < result.insight_count; i++) {
        if (result.insights[i].score_delta > 0) {
            found_positive = true;
            HU_ASSERT_TRUE(result.insights[i].observation != NULL);
            HU_ASSERT_TRUE(strstr(result.insights[i].observation, "engagement") != NULL ||
                           strstr(result.insights[i].observation, "enthusiastic") != NULL);
            break;
        }
    }
    HU_ASSERT_TRUE(found_positive);
    hu_replay_result_deinit(&result, &alloc);
}

static void replay_detects_robotic_tell(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[2] = {
        make_entry(false, "I've been really stressed about work", "12:00"),
        make_entry(true, "I understand how you feel. That must be difficult.", "12:01"),
    };
    hu_replay_result_t result = {0};
    hu_error_t err = hu_replay_analyze(&alloc, entries, 2, 300, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.insight_count >= 1);
    bool found_negative = false;
    for (size_t i = 0; i < result.insight_count; i++) {
        if (result.insights[i].score_delta < 0) {
            found_negative = true;
            HU_ASSERT_TRUE(result.insights[i].observation != NULL);
            HU_ASSERT_TRUE(strstr(result.insights[i].observation, "specificity") != NULL ||
                           strstr(result.insights[i].observation, "follow-up") != NULL ||
                           strstr(result.insights[i].observation, "robotic") != NULL);
            break;
        }
    }
    HU_ASSERT_TRUE(found_negative);
    hu_replay_result_deinit(&result, &alloc);
}

static void replay_empty_history(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_replay_result_t result = {0};
    hu_error_t err = hu_replay_analyze(&alloc, NULL, 0, 300, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(result.insight_count, 0u);
    HU_ASSERT_EQ(result.overall_score, 0);
}

static void replay_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[1] = {make_entry(true, "hi", "12:00")};
    hu_replay_result_t result = {0};

    hu_error_t err = hu_replay_analyze(NULL, entries, 1, 300, &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    err = hu_replay_analyze(&alloc, entries, 1, 300, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    err = hu_replay_analyze(&alloc, NULL, 1, 300, &result);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void replay_build_context_with_insights(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_channel_history_entry_t entries[2] = {
        make_entry(true, "that's awesome", "12:00"),
        make_entry(false, "yes!!", "12:01"),
    };
    hu_replay_result_t result = {0};
    hu_error_t err = hu_replay_analyze(&alloc, entries, 2, 300, &result);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(result.insight_count >= 1);

    size_t len = 0;
    char *ctx = hu_replay_build_context(&alloc, &result, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "Session Replay") != NULL);
    alloc.free(alloc.ctx, ctx, len + 1);
    hu_replay_result_deinit(&result, &alloc);
}

static void replay_build_context_empty_returns_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_replay_result_t result = {0};
    result.insight_count = 0;
    size_t len = 0;
    char *ctx = hu_replay_build_context(&alloc, &result, &len);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(len, 0u);
}

static void auto_tune_no_memory_returns_error(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *summary = NULL;
    size_t summary_len = 0;
    hu_error_t err = hu_replay_auto_tune(&alloc, NULL, "contact_a", 9, &summary, &summary_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void auto_tune_null_args(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *summary = NULL;
    size_t summary_len = 0;
    HU_ASSERT_EQ(hu_replay_auto_tune(NULL, NULL, NULL, 0, &summary, &summary_len),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_replay_auto_tune(&alloc, NULL, NULL, 0, NULL, &summary_len),
                 HU_ERR_INVALID_ARGUMENT);
}

static void auto_tune_empty_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    char *summary = NULL;
    size_t summary_len = 0;
    hu_error_t err = hu_replay_auto_tune(&alloc, &mem, "contact_a", 9, &summary, &summary_len);
    HU_ASSERT_EQ(err, HU_OK);

    if (summary)
        alloc.free(alloc.ctx, summary, summary_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void auto_tune_with_stored_insights(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_memory_lru_create(&alloc, 100);
    HU_ASSERT_NOT_NULL(mem.ctx);

    static const char cat_name[] = "replay_insights";
    hu_memory_category_t cat = {
        .tag = HU_MEMORY_CATEGORY_CUSTOM,
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
    hu_error_t err = hu_replay_auto_tune(&alloc, &mem, NULL, 0, &summary, &summary_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(summary_len > 0);
    HU_ASSERT_TRUE(strstr(summary, "enthusiastic") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "robotic") != NULL);

    alloc.free(alloc.ctx, summary, summary_len + 1);
    if (mem.vtable->deinit)
        mem.vtable->deinit(mem.ctx);
}

static void tune_build_context_formats(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *summary = "WHAT'S WORKING: casual humor\nWHAT TO AVOID: robotic greetings";
    size_t out_len = 0;
    char *ctx = hu_replay_tune_build_context(&alloc, summary, strlen(summary), &out_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "Tone Calibration") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "casual humor") != NULL);
    alloc.free(alloc.ctx, ctx, out_len + 1);
}

static void tune_build_context_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 0;
    char *ctx = hu_replay_tune_build_context(&alloc, NULL, 0, &out_len);
    HU_ASSERT_NULL(ctx);
}

void run_replay_tests(void) {
    HU_TEST_SUITE("replay");
    HU_RUN_TEST(replay_detects_positive_signal);
    HU_RUN_TEST(replay_detects_robotic_tell);
    HU_RUN_TEST(replay_empty_history);
    HU_RUN_TEST(replay_null_args);
    HU_RUN_TEST(replay_build_context_with_insights);
    HU_RUN_TEST(replay_build_context_empty_returns_null);

    HU_TEST_SUITE("replay — auto-tune");
    HU_RUN_TEST(auto_tune_no_memory_returns_error);
    HU_RUN_TEST(auto_tune_null_args);
    HU_RUN_TEST(auto_tune_empty_memory);
    HU_RUN_TEST(auto_tune_with_stored_insights);
    HU_RUN_TEST(tune_build_context_formats);
    HU_RUN_TEST(tune_build_context_null);
}
