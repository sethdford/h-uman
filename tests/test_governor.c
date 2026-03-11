#include "human/agent/governor.h"
#include "test_framework.h"
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void governor_init_defaults_produces_valid_budget(void) {
    hu_proactive_budget_t budget;
    memset(&budget, 0xff, sizeof(budget));

    HU_ASSERT_EQ(hu_governor_init(NULL, &budget), HU_OK);
    HU_ASSERT_EQ(budget.daily_max, 3u);
    HU_ASSERT_EQ(budget.weekly_max, 10u);
    HU_ASSERT_FLOAT_EQ(budget.relationship_multiplier, 1.0, 0.001);
    HU_ASSERT_EQ(budget.daily_used, 0u);
    HU_ASSERT_EQ(budget.weekly_used, 0u);
}

static void governor_init_custom_config(void) {
    hu_proactive_budget_config_t config = {
        .daily_max = 5,
        .weekly_max = 15,
        .relationship_multiplier = 1.5,
        .cool_off_after_unanswered = 3,
        .cool_off_hours = 48,
    };
    hu_proactive_budget_t budget;

    HU_ASSERT_EQ(hu_governor_init(&config, &budget), HU_OK);
    HU_ASSERT_EQ(budget.daily_max, 5u);
    HU_ASSERT_EQ(budget.weekly_max, 15u);
    HU_ASSERT_FLOAT_EQ(budget.relationship_multiplier, 1.5, 0.001);
    HU_ASSERT_EQ(budget.cool_off_after_unanswered, 3u);
    HU_ASSERT_EQ(budget.cool_off_hours, 48u);
}

static void governor_has_budget_initially_true(void) {
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(NULL, &budget), HU_OK);

    HU_ASSERT_TRUE(hu_governor_has_budget(&budget, 1000000ULL));
}

static void governor_has_budget_false_when_daily_exhausted(void) {
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(NULL, &budget), HU_OK);
    budget.daily_used = hu_governor_effective_daily_max(&budget);

    HU_ASSERT_FALSE(hu_governor_has_budget(&budget, 1000000ULL));
}

static void governor_has_budget_false_during_cool_off(void) {
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(NULL, &budget), HU_OK);
    budget.cool_off_until = 2000000ULL;

    HU_ASSERT_FALSE(hu_governor_has_budget(&budget, 1000000ULL));
}

static void governor_record_sent_increments_counters(void) {
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(NULL, &budget), HU_OK);
    uint64_t now = 1000000ULL;

    HU_ASSERT_EQ(hu_governor_record_sent(&budget, now), HU_OK);
    HU_ASSERT_EQ(budget.daily_used, 1u);
    HU_ASSERT_EQ(budget.weekly_used, 1u);
    HU_ASSERT_EQ(budget.unanswered_count, 1u);

    HU_ASSERT_EQ(hu_governor_record_sent(&budget, now), HU_OK);
    HU_ASSERT_EQ(budget.daily_used, 2u);
    HU_ASSERT_EQ(budget.weekly_used, 2u);
    HU_ASSERT_EQ(budget.unanswered_count, 2u);
}

static void governor_record_sent_triggers_cool_off(void) {
    hu_proactive_budget_config_t config = {
        .daily_max = 5,
        .weekly_max = 20,
        .relationship_multiplier = 1.0,
        .cool_off_after_unanswered = 2,
        .cool_off_hours = 72,
    };
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(&config, &budget), HU_OK);
    uint64_t now = 1000000ULL;

    HU_ASSERT_EQ(hu_governor_record_sent(&budget, now), HU_OK);
    HU_ASSERT_EQ(budget.cool_off_until, 0u);

    HU_ASSERT_EQ(hu_governor_record_sent(&budget, now), HU_OK);
    HU_ASSERT_TRUE(budget.cool_off_until > now);
}

static void governor_record_response_resets_unanswered(void) {
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(NULL, &budget), HU_OK);
    uint64_t now = 1000000ULL;

    hu_governor_record_sent(&budget, now);
    hu_governor_record_sent(&budget, now);
    HU_ASSERT_EQ(budget.unanswered_count, 2u);

    HU_ASSERT_EQ(hu_governor_record_response(&budget), HU_OK);
    HU_ASSERT_EQ(budget.unanswered_count, 0u);
}

static void governor_record_response_clears_cool_off(void) {
    hu_proactive_budget_config_t config = {
        .daily_max = 5,
        .weekly_max = 20,
        .relationship_multiplier = 1.0,
        .cool_off_after_unanswered = 2,
        .cool_off_hours = 72,
    };
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(&config, &budget), HU_OK);
    uint64_t now = 1000000ULL;

    hu_governor_record_sent(&budget, now);
    hu_governor_record_sent(&budget, now);
    HU_ASSERT_TRUE(budget.cool_off_until > 0);

    HU_ASSERT_EQ(hu_governor_record_response(&budget), HU_OK);
    HU_ASSERT_EQ(budget.cool_off_until, 0u);
}

static void governor_filter_allows_all_under_budget(void) {
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(NULL, &budget), HU_OK);
    budget.daily_used = 0;
    budget.weekly_used = 0;
    uint64_t now = 1000000ULL;

    double priorities[] = {0.5, 0.3};
    bool allowed[2];
    size_t allowed_count = 0;

    HU_ASSERT_EQ(hu_governor_filter_by_priority(&budget, priorities, 2, allowed, &allowed_count,
                                                now),
                 HU_OK);
    HU_ASSERT_EQ(allowed_count, 2u);
    HU_ASSERT_TRUE(allowed[0]);
    HU_ASSERT_TRUE(allowed[1]);
}

static void governor_filter_trims_to_budget(void) {
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(NULL, &budget), HU_OK);
    budget.daily_used = 1;
    budget.weekly_used = 0;
    uint64_t now = 1000000ULL;

    double priorities[] = {0.1, 0.9, 0.5, 0.3, 0.7};
    bool allowed[5];
    size_t allowed_count = 0;

    HU_ASSERT_EQ(hu_governor_filter_by_priority(&budget, priorities, 5, allowed, &allowed_count,
                                                now),
                 HU_OK);
    HU_ASSERT_EQ(allowed_count, 2u);
    HU_ASSERT_TRUE(allowed[1]);
    HU_ASSERT_TRUE(allowed[4]);
    HU_ASSERT_FALSE(allowed[0]);
    HU_ASSERT_FALSE(allowed[2]);
    HU_ASSERT_FALSE(allowed[3]);
}

static void governor_filter_blocks_all_in_cool_off(void) {
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(NULL, &budget), HU_OK);
    budget.cool_off_until = 2000000ULL;
    uint64_t now = 1000000ULL;

    double priorities[] = {0.9, 0.8};
    bool allowed[2];
    size_t allowed_count = 99;

    HU_ASSERT_EQ(hu_governor_filter_by_priority(&budget, priorities, 2, allowed, &allowed_count,
                                                now),
                 HU_OK);
    HU_ASSERT_EQ(allowed_count, 0u);
    HU_ASSERT_FALSE(allowed[0]);
    HU_ASSERT_FALSE(allowed[1]);
}

static void governor_daily_reset_on_new_day(void) {
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(NULL, &budget), HU_OK);
    budget.daily_used = 3;
    budget.weekly_used = 5;
    budget.last_reset_day = 10;
    budget.last_reset_week = 1;
    uint64_t now = 12ULL * 86400000ULL;

    double priorities[] = {0.5};
    bool allowed[1];
    size_t allowed_count = 0;

    HU_ASSERT_EQ(hu_governor_filter_by_priority(&budget, priorities, 1, allowed, &allowed_count,
                                                now),
                 HU_OK);
    HU_ASSERT_EQ(budget.daily_used, 0u);
    HU_ASSERT_EQ(allowed_count, 1u);
    HU_ASSERT_TRUE(allowed[0]);
}

static void governor_weekly_reset_on_new_week(void) {
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(NULL, &budget), HU_OK);
    budget.daily_used = 0;
    budget.weekly_used = 10;
    budget.last_reset_day = 6;
    budget.last_reset_week = 0;
    uint64_t now = 8ULL * 86400000ULL;

    double priorities[] = {0.5};
    bool allowed[1];
    size_t allowed_count = 0;

    HU_ASSERT_EQ(hu_governor_filter_by_priority(&budget, priorities, 1, allowed, &allowed_count,
                                                now),
                 HU_OK);
    HU_ASSERT_EQ(budget.weekly_used, 0u);
    HU_ASSERT_EQ(allowed_count, 1u);
}

static void governor_effective_daily_max_with_multiplier(void) {
    hu_proactive_budget_config_t config = {
        .daily_max = 3,
        .weekly_max = 10,
        .relationship_multiplier = 1.5,
        .cool_off_after_unanswered = 2,
        .cool_off_hours = 72,
    };
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(&config, &budget), HU_OK);

    HU_ASSERT_EQ(hu_governor_effective_daily_max(&budget), 4u);
}

static void governor_effective_daily_max_minimum_one(void) {
    hu_proactive_budget_config_t config = {
        .daily_max = 3,
        .weekly_max = 10,
        .relationship_multiplier = 0.0,
        .cool_off_after_unanswered = 2,
        .cool_off_hours = 72,
    };
    hu_proactive_budget_t budget;
    HU_ASSERT_EQ(hu_governor_init(&config, &budget), HU_OK);

    HU_ASSERT_EQ(hu_governor_effective_daily_max(&budget), 1u);
}

static void governor_compute_priority_weights(void) {
    double r = hu_governor_compute_priority(10, 0, 0, 0, 0);
    HU_ASSERT_FLOAT_EQ(r, 0.30, 0.001);

    r = hu_governor_compute_priority(0, 10, 0, 0, 0);
    HU_ASSERT_FLOAT_EQ(r, 0.25, 0.001);

    r = hu_governor_compute_priority(0, 0, 10, 0, 0);
    HU_ASSERT_FLOAT_EQ(r, 0.20, 0.001);

    r = hu_governor_compute_priority(0, 0, 0, 10, 0);
    HU_ASSERT_FLOAT_EQ(r, 0.15, 0.001);

    r = hu_governor_compute_priority(0, 0, 0, 0, 10);
    HU_ASSERT_FLOAT_EQ(r, 0.10, 0.001);

    r = hu_governor_compute_priority(10, 10, 10, 10, 10);
    HU_ASSERT_FLOAT_EQ(r, 1.0, 0.001);
}

static void governor_compute_priority_max_inputs(void) {
    double r = hu_governor_compute_priority(10, 10, 10, 10, 10);
    HU_ASSERT_FLOAT_EQ(r, 1.0, 0.001);
}

void run_governor_tests(void) {
    HU_TEST_SUITE("governor");
    HU_RUN_TEST(governor_init_defaults_produces_valid_budget);
    HU_RUN_TEST(governor_init_custom_config);
    HU_RUN_TEST(governor_has_budget_initially_true);
    HU_RUN_TEST(governor_has_budget_false_when_daily_exhausted);
    HU_RUN_TEST(governor_has_budget_false_during_cool_off);
    HU_RUN_TEST(governor_record_sent_increments_counters);
    HU_RUN_TEST(governor_record_sent_triggers_cool_off);
    HU_RUN_TEST(governor_record_response_resets_unanswered);
    HU_RUN_TEST(governor_record_response_clears_cool_off);
    HU_RUN_TEST(governor_filter_allows_all_under_budget);
    HU_RUN_TEST(governor_filter_trims_to_budget);
    HU_RUN_TEST(governor_filter_blocks_all_in_cool_off);
    HU_RUN_TEST(governor_daily_reset_on_new_day);
    HU_RUN_TEST(governor_weekly_reset_on_new_week);
    HU_RUN_TEST(governor_effective_daily_max_with_multiplier);
    HU_RUN_TEST(governor_effective_daily_max_minimum_one);
    HU_RUN_TEST(governor_compute_priority_weights);
    HU_RUN_TEST(governor_compute_priority_max_inputs);
}
