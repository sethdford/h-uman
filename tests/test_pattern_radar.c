#include "seaclaw/agent/pattern_radar.h"
#include "seaclaw/core/allocator.h"
#include "test_framework.h"
#include <string.h>

static void radar_tracks_topic_recurrence(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_pattern_radar_t radar;
    SC_ASSERT_EQ(sc_pattern_radar_init(&radar, alloc), SC_OK);

    const char *topic = "work stress";
    const char *ts = "2025-03-07T10:00:00";
    for (int i = 0; i < 4; i++) {
        sc_error_t err = sc_pattern_radar_observe(&radar, topic, strlen(topic),
                                                   SC_PATTERN_TOPIC_RECURRENCE, NULL, 0, ts,
                                                   strlen(ts));
        SC_ASSERT_EQ(err, SC_OK);
    }

    SC_ASSERT_EQ(radar.observation_count, 1u);
    SC_ASSERT_EQ(radar.observations[0].occurrence_count, 4u);
    SC_ASSERT_EQ(radar.observations[0].type, SC_PATTERN_TOPIC_RECURRENCE);

    sc_pattern_radar_deinit(&radar);
}

static void radar_separates_different_topics(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_pattern_radar_t radar;
    SC_ASSERT_EQ(sc_pattern_radar_init(&radar, alloc), SC_OK);

    SC_ASSERT_EQ(sc_pattern_radar_observe(&radar, "topic_a", 7, SC_PATTERN_TOPIC_RECURRENCE,
                                           NULL, 0, "ts1", 3),
                 SC_OK);
    SC_ASSERT_EQ(sc_pattern_radar_observe(&radar, "topic_b", 7, SC_PATTERN_TOPIC_RECURRENCE,
                                           NULL, 0, "ts2", 3),
                 SC_OK);

    SC_ASSERT_EQ(radar.observation_count, 2u);
    SC_ASSERT_EQ(radar.observations[0].occurrence_count, 1u);
    SC_ASSERT_EQ(radar.observations[1].occurrence_count, 1u);

    sc_pattern_radar_deinit(&radar);
}

static void radar_build_context_only_shows_patterns(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_pattern_radar_t radar;
    SC_ASSERT_EQ(sc_pattern_radar_init(&radar, alloc), SC_OK);

    /* One topic once — should NOT appear (threshold is 3) */
    SC_ASSERT_EQ(sc_pattern_radar_observe(&radar, "rare_topic", 10, SC_PATTERN_TOPIC_RECURRENCE,
                                           NULL, 0, "ts1", 3),
                 SC_OK);

    /* One topic 4 times — should appear */
    for (int i = 0; i < 4; i++) {
        SC_ASSERT_EQ(sc_pattern_radar_observe(&radar, "recurring_topic", 15,
                                               SC_PATTERN_TOPIC_RECURRENCE, NULL, 0, "ts2", 3),
                     SC_OK);
    }

    char *ctx = NULL;
    size_t ctx_len = 0;
    SC_ASSERT_EQ(sc_pattern_radar_build_context(&radar, &alloc, &ctx, &ctx_len), SC_OK);
    SC_ASSERT_NOT_NULL(ctx);
    SC_ASSERT_TRUE(strstr(ctx, "Pattern Insights") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "recurring_topic") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "4 times") != NULL);
    SC_ASSERT_TRUE(strstr(ctx, "rare_topic") == NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    sc_pattern_radar_deinit(&radar);
}

static void radar_handles_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_pattern_radar_t radar;
    SC_ASSERT_EQ(sc_pattern_radar_init(&radar, alloc), SC_OK);

    char *ctx = NULL;
    size_t ctx_len = 0;
    SC_ASSERT_EQ(sc_pattern_radar_build_context(&radar, &alloc, &ctx, &ctx_len), SC_OK);
    SC_ASSERT_NULL(ctx);
    SC_ASSERT_EQ(ctx_len, 0u);

    sc_pattern_radar_deinit(&radar);
}

void run_pattern_radar_tests(void) {
    SC_TEST_SUITE("pattern_radar");
    SC_RUN_TEST(radar_tracks_topic_recurrence);
    SC_RUN_TEST(radar_separates_different_topics);
    SC_RUN_TEST(radar_build_context_only_shows_patterns);
    SC_RUN_TEST(radar_handles_empty);
}
