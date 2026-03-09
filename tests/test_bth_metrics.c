#include "seaclaw/core/allocator.h"
#include "seaclaw/observability/bth_metrics.h"
#include "test_framework.h"
#include <stdint.h>
#include <string.h>

static void bth_metrics_init_zeros(void) {
    sc_bth_metrics_t m;
    sc_bth_metrics_init(&m);
    SC_ASSERT_EQ(m.emotions_surfaced, 0u);
    SC_ASSERT_EQ(m.facts_extracted, 0u);
    SC_ASSERT_EQ(m.commitment_followups, 0u);
    SC_ASSERT_EQ(m.pattern_insights, 0u);
    SC_ASSERT_EQ(m.emotions_promoted, 0u);
    SC_ASSERT_EQ(m.events_extracted, 0u);
    SC_ASSERT_EQ(m.mood_contexts_built, 0u);
    SC_ASSERT_EQ(m.silence_checkins, 0u);
    SC_ASSERT_EQ(m.event_followups, 0u);
    SC_ASSERT_EQ(m.starters_built, 0u);
    SC_ASSERT_EQ(m.typos_applied, 0u);
    SC_ASSERT_EQ(m.corrections_sent, 0u);
    SC_ASSERT_EQ(m.thinking_responses, 0u);
    SC_ASSERT_EQ(m.callbacks_triggered, 0u);
    SC_ASSERT_EQ(m.reactions_sent, 0u);
    SC_ASSERT_EQ(m.link_contexts, 0u);
    SC_ASSERT_EQ(m.attachment_contexts, 0u);
    SC_ASSERT_EQ(m.ab_evaluations, 0u);
    SC_ASSERT_EQ(m.ab_alternates_chosen, 0u);
    SC_ASSERT_EQ(m.replay_analyses, 0u);
    SC_ASSERT_EQ(m.egraph_contexts, 0u);
    SC_ASSERT_EQ(m.vision_descriptions, 0u);
    SC_ASSERT_EQ(m.total_turns, 0u);
}

static void bth_metrics_summary_with_data(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_bth_metrics_t m;
    sc_bth_metrics_init(&m);
    m.emotions_surfaced = 3;
    m.facts_extracted = 7;
    m.typos_applied = 2;
    m.total_turns = 100;

    size_t len = 0;
    char *summary = sc_bth_metrics_summary(&alloc, &m, &len);
    SC_ASSERT_NOT_NULL(summary);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(strstr(summary, "emotions_surfaced=3") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "facts_extracted=7") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "typos_applied=2") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "total_turns=100") != NULL);

    alloc.free(alloc.ctx, summary, len);
}

static void bth_metrics_summary_empty(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_bth_metrics_t m;
    sc_bth_metrics_init(&m);

    size_t len = 0;
    char *summary = sc_bth_metrics_summary(&alloc, &m, &len);
    SC_ASSERT_NOT_NULL(summary);
    SC_ASSERT_TRUE(len > 0);
    SC_ASSERT_TRUE(strstr(summary, "emotions_surfaced=0") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "total_turns=0") != NULL);

    alloc.free(alloc.ctx, summary, len);
}

static void bth_metrics_all_counters_addressable(void) {
    sc_bth_metrics_t m;
    sc_bth_metrics_init(&m);

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
    SC_ASSERT_EQ(sum, 276u);

    /* Summary should include all 23 non-zero counters */
    sc_allocator_t alloc = sc_system_allocator();
    size_t out_len = 0;
    char *summary = sc_bth_metrics_summary(&alloc, &m, &out_len);
    SC_ASSERT_NOT_NULL(summary);
    SC_ASSERT_TRUE(strstr(summary, "vision_descriptions=22") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "starters_built=10") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "thinking_responses=13") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "link_contexts=16") != NULL);
    alloc.free(alloc.ctx, summary, out_len);
}

static void bth_metrics_log_does_not_crash_with_zeros(void) {
    sc_bth_metrics_t m;
    sc_bth_metrics_init(&m);
    sc_bth_metrics_log(&m);
}

static void bth_metrics_summary_contains_counter_names(void) {
    sc_allocator_t alloc = sc_system_allocator();
    sc_bth_metrics_t m;
    sc_bth_metrics_init(&m);
    m.commitment_followups = 5;
    m.pattern_insights = 3;
    m.reactions_sent = 7;

    size_t len = 0;
    char *summary = sc_bth_metrics_summary(&alloc, &m, &len);
    SC_ASSERT_NOT_NULL(summary);
    SC_ASSERT_TRUE(strstr(summary, "commitment_followups=5") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "pattern_insights=3") != NULL);
    SC_ASSERT_TRUE(strstr(summary, "reactions_sent=7") != NULL);
    alloc.free(alloc.ctx, summary, len);
}

static void bth_metrics_summary_null_alloc(void) {
    sc_bth_metrics_t m;
    sc_bth_metrics_init(&m);
    size_t len = 0;
    char *summary = sc_bth_metrics_summary(NULL, &m, &len);
    SC_ASSERT_NULL(summary);
}

static void bth_metrics_summary_null_metrics(void) {
    sc_allocator_t alloc = sc_system_allocator();
    size_t len = 0;
    char *summary = sc_bth_metrics_summary(&alloc, NULL, &len);
    SC_ASSERT_NULL(summary);
}

void run_bth_metrics_tests(void) {
    SC_TEST_SUITE("bth_metrics");
    SC_RUN_TEST(bth_metrics_init_zeros);
    SC_RUN_TEST(bth_metrics_summary_with_data);
    SC_RUN_TEST(bth_metrics_summary_empty);
    SC_RUN_TEST(bth_metrics_all_counters_addressable);
    SC_RUN_TEST(bth_metrics_log_does_not_crash_with_zeros);
    SC_RUN_TEST(bth_metrics_summary_contains_counter_names);
    SC_RUN_TEST(bth_metrics_summary_null_alloc);
    SC_RUN_TEST(bth_metrics_summary_null_metrics);
}
