#include "human/core/allocator.h"
#include "human/memory/emotional_graph.h"
#include "human/memory/stm.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>

static void egraph_record_and_query(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_graph_t g;
    hu_egraph_init(&g, alloc);

    hu_error_t err = hu_egraph_record(&g, "work", 4, HU_EMOTION_JOY, 0.8);
    HU_ASSERT_EQ(err, HU_OK);

    double avg = 0.0;
    hu_emotion_tag_t tag = hu_egraph_query(&g, "work", 4, &avg);
    HU_ASSERT_EQ(tag, HU_EMOTION_JOY);
    HU_ASSERT_EQ(avg, 0.8);

    hu_egraph_deinit(&g);
}

static void egraph_running_average(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_graph_t g;
    hu_egraph_init(&g, alloc);

    HU_ASSERT_EQ(hu_egraph_record(&g, "work", 4, HU_EMOTION_JOY, 0.6), HU_OK);
    HU_ASSERT_EQ(hu_egraph_record(&g, "work", 4, HU_EMOTION_JOY, 0.8), HU_OK);

    double avg = 0.0;
    hu_emotion_tag_t tag = hu_egraph_query(&g, "work", 4, &avg);
    HU_ASSERT_EQ(tag, HU_EMOTION_JOY);
    HU_ASSERT_EQ(avg, 0.7);

    hu_egraph_deinit(&g);
}

static void egraph_multiple_emotions(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_graph_t g;
    hu_egraph_init(&g, alloc);

    HU_ASSERT_EQ(hu_egraph_record(&g, "work", 4, HU_EMOTION_JOY, 0.3), HU_OK);
    HU_ASSERT_EQ(hu_egraph_record(&g, "work", 4, HU_EMOTION_ANXIETY, 0.9), HU_OK);

    double avg = 0.0;
    hu_emotion_tag_t tag = hu_egraph_query(&g, "work", 4, &avg);
    HU_ASSERT_EQ(tag, HU_EMOTION_ANXIETY);
    HU_ASSERT_EQ(avg, 0.9);

    hu_egraph_deinit(&g);
}

static void egraph_build_context_with_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_graph_t g;
    hu_egraph_init(&g, alloc);

    HU_ASSERT_EQ(hu_egraph_record(&g, "work", 4, HU_EMOTION_ANXIETY, 0.85), HU_OK);
    HU_ASSERT_EQ(hu_egraph_record(&g, "cooking", 7, HU_EMOTION_JOY, 0.5), HU_OK);

    size_t len = 0;
    char *ctx = hu_egraph_build_context(&alloc, &g, &len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "### Emotional Topic Map") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "work") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "cooking") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "anxiety") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "joy") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "high") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "moderate") != NULL);

    alloc.free(alloc.ctx, ctx, len + 1);
    hu_egraph_deinit(&g);
}

static void egraph_build_context_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_graph_t g;
    hu_egraph_init(&g, alloc);

    size_t len = 0;
    char *ctx = hu_egraph_build_context(&alloc, &g, &len);
    HU_ASSERT_NULL(ctx);
    HU_ASSERT_EQ(len, 0);

    hu_egraph_deinit(&g);
}

static void egraph_populate_from_stm(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess", 4);
    hu_stm_record_turn(&buf, "user", 4, "Work is stressing me out", 24, 1000);
    hu_stm_turn_set_primary_topic(&buf, 0, "work", 4);
    hu_stm_turn_add_emotion(&buf, 0, HU_EMOTION_ANXIETY, 0.7);

    hu_emotional_graph_t g;
    hu_egraph_init(&g, alloc);
    hu_error_t err = hu_egraph_populate_from_stm(&g, &buf);
    HU_ASSERT_EQ(err, HU_OK);

    double avg = 0.0;
    hu_emotion_tag_t tag = hu_egraph_query(&g, "work", 4, &avg);
    HU_ASSERT_EQ(tag, HU_EMOTION_ANXIETY);
    HU_ASSERT_EQ(avg, 0.7);

    hu_egraph_deinit(&g);
    hu_stm_deinit(&buf);
}

static void egraph_case_insensitive(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_graph_t g;
    hu_egraph_init(&g, alloc);

    HU_ASSERT_EQ(hu_egraph_record(&g, "Work", 4, HU_EMOTION_JOY, 0.8), HU_OK);

    double avg = 0.0;
    hu_emotion_tag_t tag = hu_egraph_query(&g, "work", 4, &avg);
    HU_ASSERT_EQ(tag, HU_EMOTION_JOY);
    HU_ASSERT_EQ(avg, 0.8);

    HU_ASSERT_EQ(hu_egraph_record(&g, "work", 4, HU_EMOTION_JOY, 0.6), HU_OK);
    tag = hu_egraph_query(&g, "WORK", 4, &avg);
    HU_ASSERT_EQ(tag, HU_EMOTION_JOY);
    HU_ASSERT_EQ(avg, 0.7);

    hu_egraph_deinit(&g);
}

static void egraph_populate_from_stm_topic_entities(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess", 4);
    hu_stm_record_turn(&buf, "user", 4, "Cooking makes me happy", 22, 1000);
    hu_stm_turn_add_entity(&buf, 0, "cooking", 7, "topic", 5, 1);
    hu_stm_turn_add_emotion(&buf, 0, HU_EMOTION_JOY, 0.9);

    hu_emotional_graph_t g;
    hu_egraph_init(&g, alloc);
    hu_error_t err = hu_egraph_populate_from_stm(&g, &buf);
    HU_ASSERT_EQ(err, HU_OK);

    double avg = 0.0;
    hu_emotion_tag_t tag = hu_egraph_query(&g, "cooking", 7, &avg);
    HU_ASSERT_EQ(tag, HU_EMOTION_JOY);
    HU_ASSERT_EQ(avg, 0.9);

    hu_egraph_deinit(&g);
    hu_stm_deinit(&buf);
}

static void egraph_query_unknown_topic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_graph_t g;
    hu_egraph_init(&g, alloc);

    double avg = 0.0;
    hu_emotion_tag_t tag = hu_egraph_query(&g, "never_recorded", 14, &avg);
    HU_ASSERT_EQ(tag, HU_EMOTION_NEUTRAL);
    HU_ASSERT_EQ(avg, 0.0);

    hu_egraph_deinit(&g);
}

static void egraph_populate_from_stm_with_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess", 4);

    /* Record a turn with topic and emotion */
    uint64_t ts = 1000000;
    HU_ASSERT_EQ(hu_stm_record_turn(&buf, "user", 4, "I love hiking in the mountains", 30, ts),
                HU_OK);
    size_t idx = hu_stm_count(&buf) - 1;
    (void)hu_stm_turn_set_primary_topic(&buf, idx, "hiking", 6);
    (void)hu_stm_turn_add_emotion(&buf, idx, HU_EMOTION_JOY, 0.8);

    /* Build egraph from STM */
    hu_emotional_graph_t egraph;
    hu_egraph_init(&egraph, alloc);
    hu_error_t err = hu_egraph_populate_from_stm(&egraph, &buf);
    HU_ASSERT_EQ(err, HU_OK);

    /* Verify the egraph has the topic-emotion association */
    double avg = 0.0;
    hu_emotion_tag_t dominant = hu_egraph_query(&egraph, "hiking", 6, &avg);
    HU_ASSERT_EQ(dominant, HU_EMOTION_JOY);
    HU_ASSERT_TRUE(avg > 0.5);

    /* Build context and verify it mentions hiking */
    size_t ctx_len = 0;
    char *ctx = hu_egraph_build_context(&alloc, &egraph, &ctx_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(ctx_len > 0);
    HU_ASSERT_TRUE(strstr(ctx, "hiking") != NULL);

    alloc.free(alloc.ctx, ctx, ctx_len + 1);
    hu_egraph_deinit(&egraph);
    hu_stm_deinit(&buf);
}

static void egraph_populate_from_stm_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_stm_buffer_t buf;
    hu_stm_init(&buf, alloc, "sess", 4);

    hu_emotional_graph_t g;
    hu_egraph_init(&g, alloc);
    hu_error_t err = hu_egraph_populate_from_stm(&g, &buf);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(g.edge_count, 0);

    hu_egraph_deinit(&g);
    hu_stm_deinit(&buf);
}

static void egraph_init_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_error_t err = hu_egraph_init(NULL, alloc);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void egraph_record_null_topic(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_graph_t g;
    hu_egraph_init(&g, alloc);

    hu_error_t err = hu_egraph_record(&g, NULL, 0, HU_EMOTION_JOY, 0.5);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);

    hu_egraph_deinit(&g);
}

static void egraph_at_capacity(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_graph_t g;
    hu_egraph_init(&g, alloc);

    char topic[16];
    for (size_t i = 0; i < HU_EGRAPH_MAX_NODES; i++) {
        int n = snprintf(topic, sizeof(topic), "topic_%zu", i);
        HU_ASSERT_TRUE(n > 0 && (size_t)n < sizeof(topic));
        hu_error_t err = hu_egraph_record(&g, topic, (size_t)n, HU_EMOTION_JOY, 0.5);
        HU_ASSERT_EQ(err, HU_OK);
    }

    hu_error_t err = hu_egraph_record(&g, "extra_topic", 11, HU_EMOTION_JOY, 0.5);
    HU_ASSERT_EQ(err, HU_ERR_OUT_OF_MEMORY);

    hu_egraph_deinit(&g);
}

static void egraph_deinit_twice(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_emotional_graph_t g;
    hu_egraph_init(&g, alloc);
    (void)hu_egraph_record(&g, "work", 4, HU_EMOTION_JOY, 0.8);

    hu_egraph_deinit(&g);
    hu_egraph_deinit(&g);
}

void run_emotional_graph_tests(void) {
    HU_TEST_SUITE("emotional_graph");
    HU_RUN_TEST(egraph_record_and_query);
    HU_RUN_TEST(egraph_running_average);
    HU_RUN_TEST(egraph_multiple_emotions);
    HU_RUN_TEST(egraph_build_context_with_data);
    HU_RUN_TEST(egraph_build_context_empty);
    HU_RUN_TEST(egraph_populate_from_stm);
    HU_RUN_TEST(egraph_populate_from_stm_with_data);
    HU_RUN_TEST(egraph_case_insensitive);
    HU_RUN_TEST(egraph_populate_from_stm_topic_entities);
    HU_RUN_TEST(egraph_query_unknown_topic);
    HU_RUN_TEST(egraph_populate_from_stm_empty);
    HU_RUN_TEST(egraph_init_null);
    HU_RUN_TEST(egraph_record_null_topic);
    HU_RUN_TEST(egraph_at_capacity);
    HU_RUN_TEST(egraph_deinit_twice);
}
