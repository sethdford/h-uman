#include "human/agent/collab_planning.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_create_table_sql_valid(void) {
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_collab_plan_create_table_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "CREATE TABLE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "plans") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "contact_id") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "description") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "proposed_time") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "status") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "trigger_type") != NULL);
}

static void test_insert_sql_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_collab_plan_t plan = {
        .plan_id = 1,
        .contact_id = hu_strndup(&alloc, "contact_1", 9),
        .contact_id_len = 9,
        .description = hu_strndup(&alloc, "dinner at the new place", 23),
        .description_len = 23,
        .location = NULL,
        .location_len = 0,
        .proposed_time_ms = 1000000ULL,
        .status = HU_COLLAB_PLAN_PROPOSED,
        .trigger_type = HU_PLAN_TRIGGER_INTEREST_MATCH,
    };
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_collab_plan_insert_sql(&plan, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "contact_1") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "dinner at the new place") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "INSERT INTO plans") != NULL);
    hu_collab_plan_deinit(&alloc, &plan);
}

static void test_insert_sql_with_location(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_collab_plan_t plan = {
        .plan_id = 2,
        .contact_id = hu_strndup(&alloc, "user_a", 6),
        .contact_id_len = 6,
        .description = hu_strndup(&alloc, "hike", 4),
        .description_len = 4,
        .location = hu_strndup(&alloc, "Mount Trail", 11),
        .location_len = 11,
        .proposed_time_ms = 2000000ULL,
        .status = HU_COLLAB_PLAN_ACCEPTED,
        .trigger_type = HU_PLAN_TRIGGER_SEASONAL,
    };
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_collab_plan_insert_sql(&plan, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "Mount Trail") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "accepted") != NULL);
    hu_collab_plan_deinit(&alloc, &plan);
}

static void test_insert_sql_without_location(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_collab_plan_t plan = {
        .plan_id = 3,
        .contact_id = hu_strndup(&alloc, "user_a", 6),
        .contact_id_len = 6,
        .description = hu_strndup(&alloc, "coffee", 6),
        .description_len = 6,
        .location = NULL,
        .location_len = 0,
        .proposed_time_ms = 3000000ULL,
        .status = HU_COLLAB_PLAN_PROPOSED,
        .trigger_type = HU_PLAN_TRIGGER_SPONTANEOUS,
    };
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_collab_plan_insert_sql(&plan, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "NULL") != NULL);
    hu_collab_plan_deinit(&alloc, &plan);
}

static void test_insert_sql_escapes_quotes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_collab_plan_t plan = {
        .plan_id = 4,
        .contact_id = hu_strndup(&alloc, "user_a", 6),
        .contact_id_len = 6,
        .description = hu_strndup(&alloc, "O'Brien's Pub", 13),
        .description_len = 13,
        .location = NULL,
        .location_len = 0,
        .proposed_time_ms = 4000000ULL,
        .status = HU_COLLAB_PLAN_PROPOSED,
        .trigger_type = HU_PLAN_TRIGGER_INTEREST_MATCH,
    };
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_collab_plan_insert_sql(&plan, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "''") != NULL);
    hu_collab_plan_deinit(&alloc, &plan);
}

static void test_query_sql_includes_contact_id(void) {
    char buf[512];
    size_t len = 0;
    const char *contact = "contact_1";
    hu_error_t err = hu_collab_plan_query_sql(contact, 9, NULL, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "contact_1") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "WHERE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "ORDER BY proposed_time DESC") != NULL);
}

static void test_query_sql_with_status_filter(void) {
    char buf[512];
    size_t len = 0;
    const char *contact = "user_a";
    hu_collab_plan_status_t status = HU_COLLAB_PLAN_ACCEPTED;
    hu_error_t err =
        hu_collab_plan_query_sql(contact, 6, &status, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "user_a") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "accepted") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "AND status") != NULL);
}

static void test_query_sql_without_status_filter(void) {
    char buf[512];
    size_t len = 0;
    const char *contact = "user_a";
    hu_error_t err = hu_collab_plan_query_sql(contact, 6, NULL, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "user_a") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "AND status") == NULL);
}

static void test_update_status_sql_valid(void) {
    char buf[256];
    size_t len = 0;
    hu_error_t err =
        hu_collab_plan_update_status_sql(42, HU_COLLAB_PLAN_COMPLETED, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "UPDATE plans") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "status") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "completed") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "42") != NULL);
}

static void test_score_trigger_with_matching_interests(void) {
    hu_allocator_t alloc = hu_system_allocator();
    /* Exact match: reason "concert" contains interest "concert" */
    const char *reason_str = "concert";
    hu_plan_trigger_t trigger = {
        .contact_id = hu_strndup(&alloc, "user_a", 6),
        .contact_id_len = 6,
        .reason = hu_strndup(&alloc, reason_str, strlen(reason_str)),
        .reason_len = strlen(reason_str),
        .trigger_type = HU_PLAN_TRIGGER_EXTERNAL_EVENT,
        .relevance_score = 0.5,
    };
    const char *interests[] = {"concert"};
    double score = hu_collab_plan_score_trigger(&trigger, interests, 1);
    HU_ASSERT_GT(score, 0.0);
    hu_plan_trigger_deinit(&alloc, &trigger);
}

static void test_score_trigger_no_interests_returns_relevance(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_trigger_t trigger = {
        .contact_id = hu_strndup(&alloc, "user_a", 6),
        .contact_id_len = 6,
        .reason = hu_strndup(&alloc, "concert", 7),
        .reason_len = 7,
        .trigger_type = HU_PLAN_TRIGGER_EXTERNAL_EVENT,
        .relevance_score = 0.7,
    };
    double score = hu_collab_plan_score_trigger(&trigger, NULL, 0);
    HU_ASSERT_FLOAT_EQ(score, 0.7, 0.001);
    hu_plan_trigger_deinit(&alloc, &trigger);
}

static void test_score_trigger_no_match(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_trigger_t trigger = {
        .contact_id = hu_strndup(&alloc, "user_a", 6),
        .contact_id_len = 6,
        .reason = hu_strndup(&alloc, "random event", 12),
        .reason_len = 12,
        .trigger_type = HU_PLAN_TRIGGER_SPONTANEOUS,
        .relevance_score = 0.3,
    };
    const char *interests[] = {"hiking", "cooking", "gardening"};
    double score = hu_collab_plan_score_trigger(&trigger, interests, 3);
    HU_ASSERT_EQ(score, 0.0);
    hu_plan_trigger_deinit(&alloc, &trigger);
}

static void test_should_propose_close_relationship(void) {
    bool ok = hu_collab_plan_should_propose("user_a", 6, 0, 3, 0.8);
    HU_ASSERT_TRUE(ok);
}

static void test_should_propose_distant_returns_false(void) {
    bool ok = hu_collab_plan_should_propose("user_a", 6, 0, 3, 0.3);
    HU_ASSERT_FALSE(ok);
}

static void test_should_propose_over_daily_limit_returns_false(void) {
    bool ok = hu_collab_plan_should_propose("user_a", 6, 3, 3, 0.9);
    HU_ASSERT_FALSE(ok);
}

static void test_build_prompt_includes_triggers(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_trigger_t triggers[1] = {{
        .contact_id = hu_strndup(&alloc, "user_a", 6),
        .contact_id_len = 6,
        .reason = hu_strndup(&alloc, "concert tickets", 15),
        .reason_len = 15,
        .trigger_type = HU_PLAN_TRIGGER_EXTERNAL_EVENT,
        .relevance_score = 0.8,
    }};
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_collab_plan_build_prompt(&alloc, triggers, 1, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "concert tickets") != NULL);
    HU_ASSERT_TRUE(strstr(out, "COLLABORATIVE PLANNING") != NULL);
    HU_ASSERT_TRUE(strstr(out, "Triggers to consider") != NULL);
    hu_str_free(&alloc, out);
    hu_plan_trigger_deinit(&alloc, &triggers[0]);
}

static void test_build_prompt_empty_triggers(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err =
        hu_collab_plan_build_prompt(&alloc, NULL, 0, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "No triggers or recent plans") != NULL);
    hu_str_free(&alloc, out);
}

static void test_plan_deinit_handles_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_collab_plan_deinit(&alloc, NULL);
    hu_collab_plan_t plan = {0};
    hu_collab_plan_deinit(NULL, &plan);
}

static void test_trigger_deinit_handles_null(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_plan_trigger_deinit(&alloc, NULL);
    hu_plan_trigger_t trigger = {0};
    hu_plan_trigger_deinit(NULL, &trigger);
}

static void test_plan_status_str_returns_labels(void) {
    HU_ASSERT_STR_EQ(hu_collab_plan_status_str(HU_COLLAB_PLAN_PROPOSED), "proposed");
    HU_ASSERT_STR_EQ(hu_collab_plan_status_str(HU_COLLAB_PLAN_ACCEPTED), "accepted");
    HU_ASSERT_STR_EQ(hu_collab_plan_status_str(HU_COLLAB_PLAN_DECLINED), "declined");
    HU_ASSERT_STR_EQ(hu_collab_plan_status_str(HU_COLLAB_PLAN_COMPLETED), "completed");
    HU_ASSERT_STR_EQ(hu_collab_plan_status_str(HU_COLLAB_PLAN_CANCELLED), "cancelled");
    HU_ASSERT_STR_EQ(hu_collab_plan_status_str(HU_COLLAB_PLAN_EXPIRED), "expired");
}

void run_collab_planning_tests(void) {
    HU_TEST_SUITE("collab_planning");
    HU_RUN_TEST(test_create_table_sql_valid);
    HU_RUN_TEST(test_insert_sql_valid);
    HU_RUN_TEST(test_insert_sql_with_location);
    HU_RUN_TEST(test_insert_sql_without_location);
    HU_RUN_TEST(test_insert_sql_escapes_quotes);
    HU_RUN_TEST(test_query_sql_includes_contact_id);
    HU_RUN_TEST(test_query_sql_with_status_filter);
    HU_RUN_TEST(test_query_sql_without_status_filter);
    HU_RUN_TEST(test_update_status_sql_valid);
    HU_RUN_TEST(test_score_trigger_with_matching_interests);
    HU_RUN_TEST(test_score_trigger_no_interests_returns_relevance);
    HU_RUN_TEST(test_score_trigger_no_match);
    HU_RUN_TEST(test_should_propose_close_relationship);
    HU_RUN_TEST(test_should_propose_distant_returns_false);
    HU_RUN_TEST(test_should_propose_over_daily_limit_returns_false);
    HU_RUN_TEST(test_build_prompt_includes_triggers);
    HU_RUN_TEST(test_build_prompt_empty_triggers);
    HU_RUN_TEST(test_plan_deinit_handles_null);
    HU_RUN_TEST(test_trigger_deinit_handles_null);
    HU_RUN_TEST(test_plan_status_str_returns_labels);
}
