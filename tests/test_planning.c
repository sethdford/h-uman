#include "human/agent/planning.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static void planning_create_table_sql_valid(void) {
    char buf[1024];
    size_t len = 0;

    HU_ASSERT_EQ(hu_planning_create_table_sql(buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "CREATE TABLE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "plans") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "contact_id") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "activity") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "proposed_at") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "status") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "suggested_time") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "scheduled_time") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "location") != NULL);
}

static void planning_insert_sql_escapes_strings(void) {
    hu_plan_t plan = {
        .id = 1,
        .contact_id = (char *)"user_a",
        .contact_id_len = 6,
        .activity = (char *)"O'Malley's Pub",
        .activity_len = 14,
        .suggested_time = (char *)"this weekend",
        .suggested_time_len = 13,
        .location = NULL,
        .location_len = 0,
        .status = HU_PLAN_PROPOSED,
        .proposed_by_self = true,
        .proposed_at = 1000000ULL,
        .scheduled_time = 0,
        .completed_at = 0,
        .reminder_sent = false,
    };
    char buf[1024];
    size_t len = 0;

    HU_ASSERT_EQ(hu_planning_insert_sql(&plan, buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "O''Malley''s Pub") != NULL);
}

static void planning_update_status_sql_valid(void) {
    char buf[256];
    size_t len = 0;

    HU_ASSERT_EQ(hu_planning_update_status_sql(42, HU_PLAN_ACCEPTED, buf, sizeof(buf), &len),
                 HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "UPDATE plans") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "status") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "accepted") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "42") != NULL);
}

static void planning_query_sql_with_contact(void) {
    char buf[512];
    size_t len = 0;
    const char *contact = "user_a";

    HU_ASSERT_EQ(hu_planning_query_sql(contact, strlen(contact), buf, sizeof(buf), &len), HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "WHERE contact_id") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "user_a") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "ORDER BY proposed_at DESC") != NULL);
}

static void planning_count_recent_sql_valid(void) {
    char buf[512];
    size_t len = 0;
    const char *contact = "user_a";
    uint64_t since = 1000000ULL;

    HU_ASSERT_EQ(hu_planning_count_recent_sql(contact, strlen(contact), since, buf, sizeof(buf),
                                              &len),
                 HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "proposed_by_self") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "proposed_at") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "1000000") != NULL);
}

static void planning_should_propose_true_when_under_limit(void) {
    HU_ASSERT_TRUE(hu_planning_should_propose(NULL, 0, 0, 0, 1000000ULL));
}

static void planning_should_propose_false_when_at_limit(void) {
    hu_planning_config_t config = {
        .max_proposals_per_contact_per_month = 2,
        .rejection_cooldown_days = 14,
        .last_hangout_window_days = 30,
    };
    HU_ASSERT_FALSE(hu_planning_should_propose(&config, 2, 0, 0, 1000000ULL));
    HU_ASSERT_FALSE(hu_planning_should_propose(&config, 3, 0, 0, 1000000ULL));
}

static void planning_should_propose_false_during_cooldown(void) {
    hu_planning_config_t config = {
        .max_proposals_per_contact_per_month = 2,
        .rejection_cooldown_days = 14,
        .last_hangout_window_days = 30,
    };
    uint64_t now = 20ULL * 86400000ULL;      /* 20 days from epoch */
    uint64_t rejected = 15ULL * 86400000ULL; /* 5 days ago */
    HU_ASSERT_FALSE(hu_planning_should_propose(&config, 0, rejected, 0, now));
}

static void planning_should_propose_true_after_cooldown(void) {
    hu_planning_config_t config = {
        .max_proposals_per_contact_per_month = 2,
        .rejection_cooldown_days = 14,
        .last_hangout_window_days = 30,
    };
    uint64_t now = 35ULL * 86400000ULL;      /* 35 days from epoch */
    uint64_t rejected = 15ULL * 86400000ULL; /* 20 days ago */
    HU_ASSERT_TRUE(hu_planning_should_propose(&config, 0, rejected, 0, now));
}

static void planning_score_proposal_all_positive(void) {
    double s = hu_planning_score_proposal(true, true, true, true, 0.9);
    HU_ASSERT_TRUE(s > 0.8);
}

static void planning_score_proposal_no_factors(void) {
    double s = hu_planning_score_proposal(false, false, false, false, 0.0);
    HU_ASSERT_FLOAT_EQ(s, 0.0, 0.001);
}

static void planning_score_proposal_interest_match_dominant(void) {
    double s = hu_planning_score_proposal(true, false, false, false, 0.0);
    HU_ASSERT_FLOAT_EQ(s, 0.3, 0.001);
}

static void planning_build_context_with_plans(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_t plans[2] = {
        {
            .activity = (char *)"Dinner at Thai place",
            .activity_len = 20,
            .status = HU_PLAN_PROPOSED,
            .suggested_time = (char *)"this Saturday",
            .suggested_time_len = 13,
            .proposed_by_self = true,
        },
        {
            .activity = (char *)"Hiking trip",
            .activity_len = 11,
            .status = HU_PLAN_ACCEPTED,
            .suggested_time = (char *)"next weekend",
            .suggested_time_len = 12,
            .proposed_by_self = false,
        },
    };
    char *out = NULL;
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_planning_build_context(&alloc, plans, 2, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Dinner at Thai place") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Hiking trip") != NULL);
    HU_ASSERT_TRUE(strstr(out, "proposed") != NULL);
    HU_ASSERT_TRUE(strstr(out, "accepted") != NULL);
    HU_ASSERT_TRUE(strstr(out, "we proposed") != NULL);
    HU_ASSERT_TRUE(strstr(out, "they proposed") != NULL);

    hu_str_free(&alloc, out);
}

static void planning_build_context_no_plans(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;

    HU_ASSERT_EQ(hu_planning_build_context(&alloc, NULL, 0, &out, &out_len), HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "No active plans") != NULL);

    hu_str_free(&alloc, out);
}

static void planning_status_str_roundtrip(void) {
    hu_plan_status_t statuses[] = {
        HU_PLAN_PROPOSED, HU_PLAN_ACCEPTED, HU_PLAN_CONFIRMED,
        HU_PLAN_COMPLETED, HU_PLAN_CANCELLED, HU_PLAN_DECLINED,
    };
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        const char *str = hu_plan_status_str(statuses[i]);
        hu_plan_status_t out;
        HU_ASSERT_TRUE(hu_plan_status_from_str(str, &out));
        HU_ASSERT_EQ(out, statuses[i]);
    }
}

static void planning_status_from_str_unknown(void) {
    hu_plan_status_t out;
    HU_ASSERT_FALSE(hu_plan_status_from_str("garbage", &out));
    HU_ASSERT_FALSE(hu_plan_status_from_str("", &out));
    HU_ASSERT_FALSE(hu_plan_status_from_str(NULL, &out));
}

static void planning_plan_deinit_frees_all(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_t plan = {0};
    plan.contact_id = hu_strndup(&alloc, "user_a", 6);
    plan.contact_id_len = 6;
    plan.activity = hu_strndup(&alloc, "dinner", 6);
    plan.activity_len = 6;
    plan.suggested_time = hu_strndup(&alloc, "tonight", 7);
    plan.suggested_time_len = 7;
    plan.location = hu_strndup(&alloc, "downtown", 8);
    plan.location_len = 8;

    hu_plan_deinit(&alloc, &plan);

    HU_ASSERT_NULL(plan.contact_id);
    HU_ASSERT_NULL(plan.activity);
    HU_ASSERT_NULL(plan.suggested_time);
    HU_ASSERT_NULL(plan.location);
}

void run_planning_tests(void) {
    HU_TEST_SUITE("planning");
    HU_RUN_TEST(planning_create_table_sql_valid);
    HU_RUN_TEST(planning_insert_sql_escapes_strings);
    HU_RUN_TEST(planning_update_status_sql_valid);
    HU_RUN_TEST(planning_query_sql_with_contact);
    HU_RUN_TEST(planning_count_recent_sql_valid);
    HU_RUN_TEST(planning_should_propose_true_when_under_limit);
    HU_RUN_TEST(planning_should_propose_false_when_at_limit);
    HU_RUN_TEST(planning_should_propose_false_during_cooldown);
    HU_RUN_TEST(planning_should_propose_true_after_cooldown);
    HU_RUN_TEST(planning_score_proposal_all_positive);
    HU_RUN_TEST(planning_score_proposal_no_factors);
    HU_RUN_TEST(planning_score_proposal_interest_match_dominant);
    HU_RUN_TEST(planning_build_context_with_plans);
    HU_RUN_TEST(planning_build_context_no_plans);
    HU_RUN_TEST(planning_status_str_roundtrip);
    HU_RUN_TEST(planning_status_from_str_unknown);
    HU_RUN_TEST(planning_plan_deinit_frees_all);
}
