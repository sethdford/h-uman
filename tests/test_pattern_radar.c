#include "human/agent/pattern_radar.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void radar_tracks_topic_recurrence(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pattern_radar_t radar;
    HU_ASSERT_EQ(hu_pattern_radar_init(&radar, alloc), HU_OK);

    const char *topic = "work stress";
    const char *ts = "2025-03-07T10:00:00";
    for (int i = 0; i < 4; i++) {
        hu_error_t err = hu_pattern_radar_observe(&radar, topic, strlen(topic),
                                                   HU_PATTERN_TOPIC_RECURRENCE, NULL, 0, ts,
                                                   strlen(ts));
        HU_ASSERT_EQ(err, HU_OK);
    }

    HU_ASSERT_EQ(radar.observation_count, 1u);
    HU_ASSERT_EQ(radar.observations[0].occurrence_count, 4u);
    HU_ASSERT_EQ(radar.observations[0].type, HU_PATTERN_TOPIC_RECURRENCE);

    hu_pattern_radar_deinit(&radar);
}

static void radar_separates_different_topics(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pattern_radar_t radar;
    HU_ASSERT_EQ(hu_pattern_radar_init(&radar, alloc), HU_OK);

    HU_ASSERT_EQ(hu_pattern_radar_observe(&radar, "topic_a", 7, HU_PATTERN_TOPIC_RECURRENCE,
                                           NULL, 0, "ts1", 3),
                 HU_OK);
    HU_ASSERT_EQ(hu_pattern_radar_observe(&radar, "topic_b", 7, HU_PATTERN_TOPIC_RECURRENCE,
                                           NULL, 0, "ts2", 3),
                 HU_OK);

    HU_ASSERT_EQ(radar.observation_count, 2u);
    HU_ASSERT_EQ(radar.observations[0].occurrence_count, 1u);
    HU_ASSERT_EQ(radar.observations[1].occurrence_count, 1u);

    hu_pattern_radar_deinit(&radar);
}

static void radar_build_context_only_shows_patterns(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pattern_radar_t radar;
    HU_ASSERT_EQ(hu_pattern_radar_init(&radar, alloc), HU_OK);

    /* One topic once — should NOT appear (threshold is 3) */
    HU_ASSERT_EQ(hu_pattern_radar_observe(&radar, "rare_topic", 10, HU_PATTERN_TOPIC_RECURRENCE,
                                           NULL, 0, "ts1", 3),
                 HU_OK);

    /* One topic 4 times — should appear */
    for (int i = 0; i < 4; i++) {
        HU_ASSERT_EQ(hu_pattern_radar_observe(&radar, "recurring_topic", 15,
                                               HU_PATTERN_TOPIC_RECURRENCE, NULL, 0, "ts2", 3),
                     HU_OK);
    }

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_pattern_radar_build_context(&radar, &alloc, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strstr(ctx, "Pattern Insights") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "recurring_topic") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "4 times") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "rare_topic") == NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    hu_pattern_radar_deinit(&radar);
}

static void radar_handles_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pattern_radar_t radar;
    HU_ASSERT_EQ(hu_pattern_radar_init(&radar, alloc), HU_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    HU_ASSERT_EQ(hu_pattern_radar_build_context(&radar, &alloc, &ctx, &ctx_len), HU_OK);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(ctx_len, 0u);

    hu_pattern_radar_deinit(&radar);
}

static void radar_at_max_observations(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pattern_radar_t radar;
    HU_ASSERT_EQ(hu_pattern_radar_init(&radar, alloc), HU_OK);

    for (size_t i = 0; i < HU_PATTERN_MAX_OBSERVATIONS; i++) {
        char topic[24];
        int n = snprintf(topic, sizeof(topic), "topic_%zu", i);
        hu_error_t err = hu_pattern_radar_observe(&radar, topic, (size_t)n,
                                                  HU_PATTERN_TOPIC_RECURRENCE, NULL, 0, "ts", 2);
        HU_ASSERT_EQ(err, HU_OK);
    }

    char extra[24];
    int n = snprintf(extra, sizeof(extra), "topic_%u", HU_PATTERN_MAX_OBSERVATIONS);
    hu_error_t err = hu_pattern_radar_observe(&radar, extra, (size_t)n,
                                              HU_PATTERN_TOPIC_RECURRENCE, NULL, 0, "ts", 2);
    HU_ASSERT_EQ(err, HU_OK);

    hu_pattern_radar_deinit(&radar);
}

static void radar_observe_null_subject_fails(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pattern_radar_t radar;
    HU_ASSERT_EQ(hu_pattern_radar_init(&radar, alloc), HU_OK);

    hu_error_t err = hu_pattern_radar_observe(&radar, NULL, 5, HU_PATTERN_TOPIC_RECURRENCE,
                                              NULL, 0, "ts", 2);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_pattern_radar_deinit(&radar);
}

static void radar_deinit_clears_memory(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_pattern_radar_t radar;
    HU_ASSERT_EQ(hu_pattern_radar_init(&radar, alloc), HU_OK);
    HU_ASSERT_EQ(hu_pattern_radar_observe(&radar, "topic", 5, HU_PATTERN_TOPIC_RECURRENCE,
                                          NULL, 0, "ts", 2),
                 HU_OK);
    HU_ASSERT_EQ(radar.observation_count, 1u);

    hu_pattern_radar_deinit(&radar);
    HU_ASSERT_EQ(radar.observation_count, 0u);
}

void run_pattern_radar_tests(void) {
    HU_TEST_SUITE("pattern_radar");
    HU_RUN_TEST(radar_tracks_topic_recurrence);
    HU_RUN_TEST(radar_separates_different_topics);
    HU_RUN_TEST(radar_build_context_only_shows_patterns);
    HU_RUN_TEST(radar_handles_empty);
    HU_RUN_TEST(radar_at_max_observations);
    HU_RUN_TEST(radar_observe_null_subject_fails);
    HU_RUN_TEST(radar_deinit_clears_memory);
}
