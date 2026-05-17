#include "human/agent/proactive_ext.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void reciprocity_multiplier_normal(void) {
    hu_reciprocity_state_t state = {
        .initiation_ratio = 0.5,
        .unanswered_proactive = 0,
        .contact_just_reengaged = false,
    };
    double m = hu_reciprocity_budget_multiplier(&state);
    HU_ASSERT_FLOAT_EQ(m, 1.0, 0.001);
}

static void reciprocity_multiplier_high_initiation(void) {
    hu_reciprocity_state_t state = {
        .initiation_ratio = 0.8,
        .unanswered_proactive = 0,
        .contact_just_reengaged = false,
    };
    double m = hu_reciprocity_budget_multiplier(&state);
    HU_ASSERT_FLOAT_EQ(m, 0.6, 0.001);
}

static void reciprocity_multiplier_cooloff(void) {
    hu_reciprocity_state_t state = {
        .initiation_ratio = 0.5,
        .unanswered_proactive = 3,
        .contact_just_reengaged = false,
    };
    double m = hu_reciprocity_budget_multiplier(&state);
    HU_ASSERT_FLOAT_EQ(m, 0.0, 0.001);
}

static void reciprocity_multiplier_reengaged(void) {
    hu_reciprocity_state_t state = {
        .initiation_ratio = 0.5,
        .unanswered_proactive = 0,
        .contact_just_reengaged = true,
    };
    double m = hu_reciprocity_budget_multiplier(&state);
    HU_ASSERT_TRUE(m > 1.0);
    HU_ASSERT_FLOAT_EQ(m, 1.5, 0.001);
}

static void busyness_multiplier_normal(void) {
    hu_busyness_state_t state = {
        .calendar_busy = false,
        .life_sim_stressed = false,
        .seed = 1,
    };
    double m = hu_busyness_budget_multiplier(&state);
    HU_ASSERT_TRUE(m >= 0.1 && m <= 1.0);
    HU_ASSERT_FLOAT_EQ(m, 1.0, 0.001);
}

static void busyness_multiplier_calendar_busy(void) {
    hu_busyness_state_t state = {
        .calendar_busy = true,
        .life_sim_stressed = false,
        .seed = 99999u,
    };
    double m = hu_busyness_budget_multiplier(&state);
    HU_ASSERT_FLOAT_EQ(m, 0.5, 0.001);
}

static void busyness_multiplier_stressed(void) {
    hu_busyness_state_t state = {
        .calendar_busy = false,
        .life_sim_stressed = true,
        .seed = 99999u,
    };
    double m = hu_busyness_budget_multiplier(&state);
    HU_ASSERT_FLOAT_EQ(m, 0.7, 0.001);
}

static void busyness_multiplier_combined(void) {
    hu_busyness_state_t state = {
        .calendar_busy = true,
        .life_sim_stressed = true,
        .seed = 99999u,
    };
    double m = hu_busyness_budget_multiplier(&state);
    HU_ASSERT_FLOAT_EQ(m, 0.35, 0.001);
}

static void disclosure_decide_ambiguous_ask(void) {
    hu_disclosure_action_t a = hu_disclosure_decide(0.5, 0);
    HU_ASSERT_EQ(a, HU_DISCLOSE_ASK_FIRST);
}

static void disclosure_decide_ambiguous_tell(void) {
    hu_disclosure_action_t a = hu_disclosure_decide(0.5, 42);
    HU_ASSERT_EQ(a, HU_DISCLOSE_TELL_NATURALLY);
}

static void disclosure_decide_ambiguous_skip(void) {
    hu_disclosure_action_t a = hu_disclosure_decide(0.5, 123);
    HU_ASSERT_EQ(a, HU_DISCLOSE_SKIP);
}

static void disclosure_decide_clear_confidence(void) {
    hu_disclosure_action_t a = hu_disclosure_decide(0.9, 0);
    HU_ASSERT_EQ(a, HU_DISCLOSE_TELL_NATURALLY);
}

static void disclosure_build_prefix_ask(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_disclosure_build_prefix(&alloc, HU_DISCLOSE_ASK_FIRST, "new job", 7, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "wait did I tell you about") != NULL);
    HU_ASSERT_TRUE(strstr(out, "new job") != NULL);
    HU_ASSERT_TRUE(strstr(out, "?") != NULL);
    hu_str_free(&alloc, out);
}

static void disclosure_build_prefix_skip(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_disclosure_build_prefix(&alloc, HU_DISCLOSE_SKIP, "topic", 5, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0);
}

static void curiosity_query_sql_valid(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_curiosity_query_sql("user_a", 6, 14, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "SELECT") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "topic_baselines") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "user_a") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "contact_id") != NULL);
}

/* 2026-05-16 P4-1: the F30 curiosity SELECT must gate on last_checkin_sent_at
 * (NULL OR < now-86400) so the same topic doesn't re-fire a check-in every
 * hour. Pre-fix the SELECT had no such gate. */
static void curiosity_query_sql_gates_on_last_checkin(void) {
    char buf[512];
    size_t len = 0;
    HU_ASSERT_EQ(hu_curiosity_query_sql("user_a", 6, 14, buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "last_checkin_sent_at IS NULL") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "last_checkin_sent_at <") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "86400") != NULL);
}

static void curiosity_score_optimal_range(void) {
    double s = hu_curiosity_score(5, 1, false);
    HU_ASSERT_TRUE(s > 0.8);
    HU_ASSERT_TRUE(s <= 1.0);
}

static void curiosity_score_too_recent(void) {
    double s = hu_curiosity_score(1, 1, false);
    HU_ASSERT_FLOAT_EQ(s, 0.4, 0.01);
}

static void callback_query_sql_valid(void) {
    char buf[512];
    size_t len = 0;
    uint64_t min_ms = 86400ULL * 1000;      /* 1 day */
    uint64_t max_ms = 14ULL * 86400 * 1000; /* 14 days */
    hu_error_t err = hu_callback_query_sql("user_a", 6, min_ms, max_ms, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "SELECT") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "micro_moments") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "user_a") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "created_at") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "BETWEEN") != NULL);
}

static void callback_score_sweet_spot(void) {
    double s = hu_callback_score(5, 0.9, true);
    HU_ASSERT_TRUE(s > 0.9);
    HU_ASSERT_TRUE(s <= 1.0);
}

static void proactive_ext_build_prompt_with_data(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_curiosity_topic_t curiosity = {
        .topic = hu_strndup(&alloc, "new apartment", 13),
        .topic_len = 13,
        .prompt = hu_strndup(&alloc, "hey how's the new place?", 24),
        .prompt_len = 24,
        .relevance = 0.85,
    };
    hu_callback_opportunity_t callback = {
        .topic = hu_strndup(&alloc, "their interview last week", 25),
        .topic_len = 25,
        .original_context = hu_strndup(&alloc, "had a big interview", 18),
        .original_context_len = 18,
        .mentioned_at = 0,
        .callback_score = 0.90,
    };
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_proactive_ext_build_prompt(&alloc, &curiosity, 1, &callback, 1, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "FOLLOW-UP OPPORTUNITIES") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Curiosity") != NULL);
    HU_ASSERT_TRUE(strstr(out, "new apartment") != NULL);
    HU_ASSERT_TRUE(strstr(out, "0.85") != NULL);
    HU_ASSERT_TRUE(strstr(out, "hey how's the new place?") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Callback") != NULL);
    HU_ASSERT_TRUE(strstr(out, "their interview last week") != NULL);
    HU_ASSERT_TRUE(strstr(out, "0.90") != NULL);
    HU_ASSERT_TRUE(strstr(out, "weave in naturally") != NULL);
    hu_str_free(&alloc, out);
    hu_curiosity_topic_deinit(&alloc, &curiosity);
    hu_callback_opportunity_deinit(&alloc, &callback);
}

void run_proactive_ext_tests(void) {
    HU_TEST_SUITE("proactive_ext");
    HU_RUN_TEST(reciprocity_multiplier_normal);
    HU_RUN_TEST(reciprocity_multiplier_high_initiation);
    HU_RUN_TEST(reciprocity_multiplier_cooloff);
    HU_RUN_TEST(reciprocity_multiplier_reengaged);
    HU_RUN_TEST(busyness_multiplier_normal);
    HU_RUN_TEST(busyness_multiplier_calendar_busy);
    HU_RUN_TEST(busyness_multiplier_stressed);
    HU_RUN_TEST(busyness_multiplier_combined);
    HU_RUN_TEST(disclosure_decide_ambiguous_ask);
    HU_RUN_TEST(disclosure_decide_ambiguous_tell);
    HU_RUN_TEST(disclosure_decide_ambiguous_skip);
    HU_RUN_TEST(disclosure_decide_clear_confidence);
    HU_RUN_TEST(disclosure_build_prefix_ask);
    HU_RUN_TEST(disclosure_build_prefix_skip);
    HU_RUN_TEST(curiosity_query_sql_valid);
    HU_RUN_TEST(curiosity_query_sql_gates_on_last_checkin);
    HU_RUN_TEST(curiosity_score_optimal_range);
    HU_RUN_TEST(curiosity_score_too_recent);
    HU_RUN_TEST(callback_query_sql_valid);
    HU_RUN_TEST(callback_score_sweet_spot);
    HU_RUN_TEST(proactive_ext_build_prompt_with_data);
}
