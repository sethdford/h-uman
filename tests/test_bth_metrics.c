#include "human/core/allocator.h"
#include "human/observability/bth_metrics.h"
#include "test_framework.h"
#include <stdint.h>
#include <string.h>

static void bth_metrics_init_zeros(void) {
    hu_bth_metrics_t m;
    hu_bth_metrics_init(&m);
    HU_ASSERT_EQ(m.emotions_surfaced, 0u);
    HU_ASSERT_EQ(m.facts_extracted, 0u);
    HU_ASSERT_EQ(m.commitment_followups, 0u);
    HU_ASSERT_EQ(m.pattern_insights, 0u);
    HU_ASSERT_EQ(m.emotions_promoted, 0u);
    HU_ASSERT_EQ(m.events_extracted, 0u);
    HU_ASSERT_EQ(m.mood_contexts_built, 0u);
    HU_ASSERT_EQ(m.silence_checkins, 0u);
    HU_ASSERT_EQ(m.event_followups, 0u);
    HU_ASSERT_EQ(m.starters_built, 0u);
    HU_ASSERT_EQ(m.typos_applied, 0u);
    HU_ASSERT_EQ(m.corrections_sent, 0u);
    HU_ASSERT_EQ(m.thinking_responses, 0u);
    HU_ASSERT_EQ(m.callbacks_triggered, 0u);
    HU_ASSERT_EQ(m.reactions_sent, 0u);
    HU_ASSERT_EQ(m.link_contexts, 0u);
    HU_ASSERT_EQ(m.attachment_contexts, 0u);
    HU_ASSERT_EQ(m.ab_evaluations, 0u);
    HU_ASSERT_EQ(m.ab_alternates_chosen, 0u);
    HU_ASSERT_EQ(m.replay_analyses, 0u);
    HU_ASSERT_EQ(m.egraph_contexts, 0u);
    HU_ASSERT_EQ(m.vision_descriptions, 0u);
    HU_ASSERT_EQ(m.skills_applied, 0u);
    HU_ASSERT_EQ(m.skills_evolved, 0u);
    HU_ASSERT_EQ(m.skills_retired, 0u);
    HU_ASSERT_EQ(m.reflections_daily, 0u);
    HU_ASSERT_EQ(m.reflections_weekly, 0u);
    HU_ASSERT_EQ(m.total_turns, 0u);
}

static void bth_metrics_summary_with_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bth_metrics_t m;
    hu_bth_metrics_init(&m);
    m.emotions_surfaced = 3;
    m.facts_extracted = 7;
    m.typos_applied = 2;
    m.total_turns = 100;

    size_t len = 0;
    char *summary = hu_bth_metrics_summary(&alloc, &m, &len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(summary, "emotions_surfaced=3") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "facts_extracted=7") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "typos_applied=2") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "total_turns=100") != NULL);

    alloc.free(alloc.ctx, summary, len);
}

static void bth_metrics_summary_empty(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bth_metrics_t m;
    hu_bth_metrics_init(&m);

    size_t len = 0;
    char *summary = hu_bth_metrics_summary(&alloc, &m, &len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(summary, "emotions_surfaced=0") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "total_turns=0") != NULL);

    alloc.free(alloc.ctx, summary, len);
}

static void bth_metrics_all_counters_addressable(void) {
    hu_bth_metrics_t m;
    hu_bth_metrics_init(&m);

    /* Set each counter individually and verify */
    m.emotions_surfaced = 1;
    m.facts_extracted = 2;
    m.commitment_followups = 3;
    m.pattern_insights = 4;
    m.emotions_promoted = 5;
    m.events_extracted = 6;
    m.mood_contexts_built = 7;
    m.silence_checkins = 8;
    m.event_followups = 9;
    m.starters_built = 10;
    m.typos_applied = 11;
    m.corrections_sent = 12;
    m.thinking_responses = 13;
    m.callbacks_triggered = 14;
    m.reactions_sent = 15;
    m.link_contexts = 16;
    m.attachment_contexts = 17;
    m.ab_evaluations = 18;
    m.ab_alternates_chosen = 19;
    m.replay_analyses = 20;
    m.egraph_contexts = 21;
    m.vision_descriptions = 22;
    m.total_turns = 23;

    /* Verify none overlapped (total should be sum 1..23 = 276) */
    uint32_t sum = m.emotions_surfaced + m.facts_extracted + m.commitment_followups +
                   m.pattern_insights + m.emotions_promoted + m.events_extracted +
                   m.mood_contexts_built + m.silence_checkins + m.event_followups +
                   m.starters_built + m.typos_applied + m.corrections_sent + m.thinking_responses +
                   m.callbacks_triggered + m.reactions_sent + m.link_contexts +
                   m.attachment_contexts + m.ab_evaluations + m.ab_alternates_chosen +
                   m.replay_analyses + m.egraph_contexts + m.vision_descriptions + m.total_turns;
    HU_ASSERT_EQ(sum, 276u);

    /* Summary should include all 23 non-zero counters */
    hu_allocator_t alloc = hu_system_allocator();
    size_t out_len = 0;
    char *summary = hu_bth_metrics_summary(&alloc, &m, &out_len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(strstr(summary, "vision_descriptions=22") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "starters_built=10") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "thinking_responses=13") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "link_contexts=16") != NULL);
    alloc.free(alloc.ctx, summary, out_len);
}

static void bth_metrics_log_does_not_crash_with_zeros(void) {
    hu_bth_metrics_t m;
    hu_bth_metrics_init(&m);
    hu_bth_metrics_log(&m);
}

static void bth_metrics_summary_contains_counter_names(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_bth_metrics_t m;
    hu_bth_metrics_init(&m);
    m.commitment_followups = 5;
    m.pattern_insights = 3;
    m.reactions_sent = 7;

    size_t len = 0;
    char *summary = hu_bth_metrics_summary(&alloc, &m, &len);
    HU_ASSERT_NOT_NULL(summary);
    HU_ASSERT_TRUE(strstr(summary, "commitment_followups=5") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "pattern_insights=3") != NULL);
    HU_ASSERT_TRUE(strstr(summary, "reactions_sent=7") != NULL);
    alloc.free(alloc.ctx, summary, len);
}

static void bth_metrics_summary_null_alloc(void) {
    hu_bth_metrics_t m;
    hu_bth_metrics_init(&m);
    size_t len = 0;
    char *summary = hu_bth_metrics_summary(NULL, &m, &len);
    HU_ASSERT_NULL(summary);
}

static void bth_metrics_summary_null_metrics(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 0;
    char *summary = hu_bth_metrics_summary(&alloc, NULL, &len);
    HU_ASSERT_NULL(summary);
}

void run_bth_metrics_tests(void) {
    HU_TEST_SUITE("bth_metrics");
    HU_RUN_TEST(bth_metrics_init_zeros);
    HU_RUN_TEST(bth_metrics_summary_with_data);
    HU_RUN_TEST(bth_metrics_summary_empty);
    HU_RUN_TEST(bth_metrics_all_counters_addressable);
    HU_RUN_TEST(bth_metrics_log_does_not_crash_with_zeros);
    HU_RUN_TEST(bth_metrics_summary_contains_counter_names);
    HU_RUN_TEST(bth_metrics_summary_null_alloc);
    HU_RUN_TEST(bth_metrics_summary_null_metrics);
}
