#include "human/core/allocator.h"
#include "human/core/string.h"
#include "human/intelligence/skill_system.h"
#include "test_framework.h"
#include <string.h>

static void test_skills_create_table_sql_produces_valid_sql(void) {
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_skills_create_table_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "CREATE TABLE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "learned_skills") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "trigger") != NULL);
}

static void test_skills_insert_sql_produces_valid_insert(void) {
    hu_learned_skill_t skill = {
        .id = 0,
        .name = "parse_json",
        .name_len = 10,
        .description = "Parse JSON strings",
        .description_len = 18,
        .trigger = "user asks for json parsing",
        .trigger_len = 27,
        .strategy = "use hu_json_parse",
        .strategy_len = 16,
        .status = HU_SKILL_EMERGING,
        .success_rate = 0.5,
        .usage_count = 0,
        .learned_at = 1000,
        .last_used = 1000,
        .parent_skill_id = 0,
    };
    char buf[1024];
    size_t len = 0;
    hu_error_t err = hu_skills_insert_sql(&skill, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(len > 0);
    HU_ASSERT_TRUE(strstr(buf, "INSERT INTO learned_skills") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "parse_json") != NULL);
}

static void test_skills_update_usage_sql_produces_valid_update(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_skills_update_usage_sql(42, 0.85, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "UPDATE learned_skills") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "0.85") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "42") != NULL);
}

static void test_skills_query_active_sql_excludes_retired(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_skills_query_active_sql(buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "status != 4") != NULL);
}

static void test_skills_query_by_trigger_sql_uses_trigger(void) {
    const char *trigger = "parse json";
    char buf[512];
    size_t len = 0;
    hu_error_t err =
        hu_skills_query_by_trigger_sql(trigger, strlen(trigger), buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "INSTR") != NULL);
}

static void test_skills_retire_sql_sets_status_four(void) {
    char buf[256];
    size_t len = 0;
    hu_error_t err = hu_skills_retire_sql(7, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "status = 4") != NULL);
}

static void test_skill_trigger_match_full_overlap_returns_one(void) {
    const char *trigger = "parse json";
    const char *situation = "parse json request";
    double score = hu_skill_trigger_match(trigger, strlen(trigger), situation, strlen(situation));
    HU_ASSERT_TRUE(score >= 0.99);
}

static void test_skill_trigger_match_partial_overlap_returns_fraction(void) {
    const char *trigger = "parse json validate";
    const char *situation = "parse json";
    double score = hu_skill_trigger_match(trigger, strlen(trigger), situation, strlen(situation));
    HU_ASSERT_TRUE(score > 0.0);
    HU_ASSERT_TRUE(score < 1.0);
}

static void test_skill_trigger_match_no_overlap_returns_zero(void) {
    const char *trigger = "send email";
    const char *situation = "parse json";
    double score = hu_skill_trigger_match(trigger, strlen(trigger), situation, strlen(situation));
    HU_ASSERT_EQ((long long)(score * 1000), 0);
}

static void test_skill_refine_success_first_use_success(void) {
    double r = hu_skill_refine_success(0.5, true, 0);
    HU_ASSERT_TRUE(r > 0.5);
}

static void test_skill_refine_success_first_use_failure(void) {
    double r = hu_skill_refine_success(0.5, false, 0);
    HU_ASSERT_TRUE(r < 0.5);
}

static void test_skill_transfer_score_same_context_high(void) {
    const char *src = "parse json in C";
    const char *tgt = "parse json in C";
    double s = hu_skill_transfer_score(src, strlen(src), tgt, strlen(tgt), 0.9);
    HU_ASSERT_TRUE(s > 0.2);
}

static void test_skill_transfer_score_different_context_low(void) {
    const char *src = "parse json";
    const char *tgt = "send email";
    double s = hu_skill_transfer_score(src, strlen(src), tgt, strlen(tgt), 0.9);
    HU_ASSERT_TRUE(s < 0.5);
}

static void test_skill_should_retire_low_success_high_usage(void) {
    bool r = hu_skill_should_retire(0.2, 15, 1000, 2000);
    HU_ASSERT_TRUE(r);
}

static void test_skill_should_retire_unused_30_days(void) {
    uint64_t ms_per_day = 86400000ULL;
    uint64_t now = 60ULL * ms_per_day;
    uint64_t last = 20ULL * ms_per_day;
    bool r = hu_skill_should_retire(0.8, 5, last, now);
    HU_ASSERT_TRUE(r);
}

static void test_skill_should_not_retire_healthy_skill(void) {
    bool r = hu_skill_should_retire(0.8, 20, 1000, 2000);
    HU_ASSERT_FALSE(r);
}

static void test_skill_chain_query_sql_filters_by_parent(void) {
    char buf[512];
    size_t len = 0;
    hu_error_t err = hu_skill_chain_query_sql(10, buf, sizeof(buf), &len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "parent_skill_id = 10") != NULL);
}

static void test_meta_learning_compute_learning_rate(void) {
    hu_meta_learning_state_t s =
        hu_meta_learning_compute(10, 3, 100, 80);
    HU_ASSERT_EQ((long long)(s.learning_rate * 100), 80);
}

static void test_skill_status_str_all_values(void) {
    HU_ASSERT_STR_EQ(hu_skill_status_str(HU_SKILL_EMERGING), "emerging");
    HU_ASSERT_STR_EQ(hu_skill_status_str(HU_SKILL_DEVELOPING), "developing");
    HU_ASSERT_STR_EQ(hu_skill_status_str(HU_SKILL_PROFICIENT), "proficient");
    HU_ASSERT_STR_EQ(hu_skill_status_str(HU_SKILL_MASTERED), "mastered");
    HU_ASSERT_STR_EQ(hu_skill_status_str(HU_SKILL_RETIRED), "retired");
}

static void test_skills_build_prompt_includes_skills(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_learned_skill_t skill = {
        .name = "parse",
        .name_len = 5,
        .trigger = "json",
        .trigger_len = 4,
        .strategy = "use hu_json",
        .strategy_len = 10,
    };
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_skills_build_prompt(&alloc, &skill, 1, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "LEARNED SKILLS") != NULL);
    HU_ASSERT_TRUE(strstr(out, "parse") != NULL);
    hu_str_free(&alloc, out);
}

void run_skill_system_tests(void) {
    HU_TEST_SUITE("skill_system");
    HU_RUN_TEST(test_skills_create_table_sql_produces_valid_sql);
    HU_RUN_TEST(test_skills_insert_sql_produces_valid_insert);
    HU_RUN_TEST(test_skills_update_usage_sql_produces_valid_update);
    HU_RUN_TEST(test_skills_query_active_sql_excludes_retired);
    HU_RUN_TEST(test_skills_query_by_trigger_sql_uses_trigger);
    HU_RUN_TEST(test_skills_retire_sql_sets_status_four);
    HU_RUN_TEST(test_skill_trigger_match_full_overlap_returns_one);
    HU_RUN_TEST(test_skill_trigger_match_partial_overlap_returns_fraction);
    HU_RUN_TEST(test_skill_trigger_match_no_overlap_returns_zero);
    HU_RUN_TEST(test_skill_refine_success_first_use_success);
    HU_RUN_TEST(test_skill_refine_success_first_use_failure);
    HU_RUN_TEST(test_skill_transfer_score_same_context_high);
    HU_RUN_TEST(test_skill_transfer_score_different_context_low);
    HU_RUN_TEST(test_skill_should_retire_low_success_high_usage);
    HU_RUN_TEST(test_skill_should_retire_unused_30_days);
    HU_RUN_TEST(test_skill_should_not_retire_healthy_skill);
    HU_RUN_TEST(test_skill_chain_query_sql_filters_by_parent);
    HU_RUN_TEST(test_meta_learning_compute_learning_rate);
    HU_RUN_TEST(test_skill_status_str_all_values);
    HU_RUN_TEST(test_skills_build_prompt_includes_skills);
}
