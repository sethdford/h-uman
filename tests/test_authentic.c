#include "human/context/authentic.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <string.h>

static void select_zero_probabilities_returns_none(void) {
    hu_authentic_config_t config = {0};
    hu_authentic_behavior_t b =
        hu_authentic_select(&config, 0.9, false, 12345u);
    HU_ASSERT_EQ(b, HU_AUTH_NONE);
}

static void select_high_closeness_serious_topic_suppresses_gossip(void) {
    hu_authentic_config_t config = {0};
    config.narration_probability = 0.0;
    config.gossip_probability = 1.0;
    config.random_thought_probability = 0.0;
    config.imperfection_probability = 0.0;
    /* With serious_topic, gossip/random_thought/imperfection are suppressed */
    hu_authentic_behavior_t b =
        hu_authentic_select(&config, 0.9, true, 999u);
    HU_ASSERT_NEQ(b, HU_AUTH_GOSSIP);
    HU_ASSERT_NEQ(b, HU_AUTH_RANDOM_THOUGHT);
    HU_ASSERT_NEQ(b, HU_AUTH_IMPERFECTION);
}

static void select_low_closeness_suppresses_existential(void) {
    hu_authentic_config_t config = {0};
    config.existential_probability = 1.0;
    config.guilt_probability = 0.0;
    config.embodiment_probability = 0.0;
    /* closeness < 0.5 suppresses existential, guilt, embodiment */
    hu_authentic_behavior_t b =
        hu_authentic_select(&config, 0.3, false, 111u);
    HU_ASSERT_NEQ(b, HU_AUTH_EXISTENTIAL);
    HU_ASSERT_NEQ(b, HU_AUTH_GUILT);
    HU_ASSERT_NEQ(b, HU_AUTH_EMBODIMENT);
}

static void select_resistance_only_high_closeness(void) {
    hu_authentic_config_t config = {0};
    config.resistance_probability = 1.0;
    hu_authentic_behavior_t b_low =
        hu_authentic_select(&config, 0.5, false, 1u);
    hu_authentic_behavior_t b_high =
        hu_authentic_select(&config, 0.8, false, 1u);
    HU_ASSERT_NEQ(b_low, HU_AUTH_RESISTANCE);
    HU_ASSERT_EQ(b_high, HU_AUTH_RESISTANCE);
}

static void select_deterministic_with_seed(void) {
    hu_authentic_config_t config = {0};
    config.narration_probability = 0.5;
    config.complaining_probability = 0.5;
    hu_authentic_behavior_t a = hu_authentic_select(&config, 0.9, false, 42u);
    hu_authentic_behavior_t b = hu_authentic_select(&config, 0.9, false, 42u);
    HU_ASSERT_EQ(a, b);
}

static void build_directive_narration_returns_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_authentic_build_directive(&alloc, HU_AUTH_NARRATION,
                                                   NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(out, "AUTHENTIC") != NULL);
    HU_ASSERT_TRUE(strstr(out, "gym") != NULL);
    hu_str_free(&alloc, out);
}

static void build_directive_embodiment_returns_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_authentic_build_directive(&alloc, HU_AUTH_EMBODIMENT,
                                                   NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "physical") != NULL);
    hu_str_free(&alloc, out);
}

static void build_directive_life_thread_with_context_includes(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *ctx = "moving to a new apartment next week";
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_authentic_build_directive(&alloc, HU_AUTH_LIFE_THREAD,
                                                   ctx, strlen(ctx), &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "apartment") != NULL);
    HU_ASSERT_TRUE(strstr(out, "AUTHENTIC") != NULL);
    hu_str_free(&alloc, out);
}

static void build_directive_life_thread_empty_uses_default(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_authentic_build_directive(&alloc, HU_AUTH_LIFE_THREAD,
                                                   NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "narrative") != NULL);
    hu_str_free(&alloc, out);
}

static void build_directive_none_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 1;
    hu_error_t err = hu_authentic_build_directive(&alloc, HU_AUTH_NONE,
                                                   NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0u);
}

static void build_directive_bad_day_returns_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_authentic_build_directive(&alloc, HU_AUTH_BAD_DAY,
                                                   NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "rough day") != NULL);
    hu_str_free(&alloc, out);
}

static void life_thread_create_table_sql_valid(void) {
    char buf[512];
    size_t out_len = 0;
    hu_error_t err = hu_life_thread_create_table_sql(buf, sizeof(buf), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(buf, "CREATE TABLE") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "life_threads") != NULL);
}

static void life_thread_insert_sql_valid(void) {
    char buf[512];
    size_t out_len = 0;
    hu_error_t err = hu_life_thread_insert_sql("coffee with Alex", 16, 1700000000000ull,
                                                buf, sizeof(buf), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(out_len > 0);
    HU_ASSERT_TRUE(strstr(buf, "INSERT INTO") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "coffee") != NULL);
}

static void life_thread_insert_sql_escapes_quotes(void) {
    char buf[512];
    size_t out_len = 0;
    hu_error_t err = hu_life_thread_insert_sql("it's a test", 11, 1700000000000ull,
                                                buf, sizeof(buf), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "''") != NULL);
}

static void life_thread_query_active_sql_valid(void) {
    char buf[256];
    size_t out_len = 0;
    hu_error_t err = hu_life_thread_query_active_sql(buf, sizeof(buf), &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_TRUE(strstr(buf, "SELECT") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "active=1") != NULL);
}

static void is_bad_day_active_within_duration_returns_true(void) {
    uint64_t start = 1000000ull;
    uint64_t now = start + (4 * 3600 * 1000);
    bool ok = hu_authentic_is_bad_day(true, start, now, 8);
    HU_ASSERT_TRUE(ok);
}

static void is_bad_day_active_expired_returns_false(void) {
    uint64_t start = 1000000ull;
    uint64_t now = start + (10 * 3600 * 1000);
    bool ok = hu_authentic_is_bad_day(true, start, now, 8);
    HU_ASSERT_FALSE(ok);
}

static void is_bad_day_inactive_returns_false(void) {
    bool ok = hu_authentic_is_bad_day(false, 0, 1000, 8);
    HU_ASSERT_FALSE(ok);
}

static void bad_day_build_directive_returns_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_bad_day_build_directive(&alloc, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "rough day") != NULL);
    hu_str_free(&alloc, out);
}

static void behavior_str_returns_expected(void) {
    HU_ASSERT_STR_EQ(hu_authentic_behavior_str(HU_AUTH_NARRATION), "narration");
    HU_ASSERT_STR_EQ(hu_authentic_behavior_str(HU_AUTH_GOSSIP), "gossip");
    HU_ASSERT_STR_EQ(hu_authentic_behavior_str(HU_AUTH_NONE), "none");
}

static void state_deinit_clears_context(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_authentic_state_t s = {0};
    s.context = hu_strndup(&alloc, "test context", 12);
    s.context_len = 12;
    s.active = HU_AUTH_NARRATION;
    hu_authentic_state_deinit(&alloc, &s);
    HU_ASSERT_NULL(s.context);
    HU_ASSERT_EQ(s.context_len, 0u);
    HU_ASSERT_EQ(s.active, HU_AUTH_NONE);
}

static void build_directive_null_alloc_returns_error(void) {
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_authentic_build_directive(NULL, HU_AUTH_NARRATION,
                                                   NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

void run_authentic_tests(void) {
    HU_TEST_SUITE("authentic");
    HU_RUN_TEST(select_zero_probabilities_returns_none);
    HU_RUN_TEST(select_high_closeness_serious_topic_suppresses_gossip);
    HU_RUN_TEST(select_low_closeness_suppresses_existential);
    HU_RUN_TEST(select_resistance_only_high_closeness);
    HU_RUN_TEST(select_deterministic_with_seed);
    HU_RUN_TEST(build_directive_narration_returns_valid);
    HU_RUN_TEST(build_directive_embodiment_returns_valid);
    HU_RUN_TEST(build_directive_life_thread_with_context_includes);
    HU_RUN_TEST(build_directive_life_thread_empty_uses_default);
    HU_RUN_TEST(build_directive_none_returns_ok);
    HU_RUN_TEST(build_directive_bad_day_returns_valid);
    HU_RUN_TEST(build_directive_null_alloc_returns_error);
    HU_RUN_TEST(life_thread_create_table_sql_valid);
    HU_RUN_TEST(life_thread_insert_sql_valid);
    HU_RUN_TEST(life_thread_insert_sql_escapes_quotes);
    HU_RUN_TEST(life_thread_query_active_sql_valid);
    HU_RUN_TEST(is_bad_day_active_within_duration_returns_true);
    HU_RUN_TEST(is_bad_day_active_expired_returns_false);
    HU_RUN_TEST(is_bad_day_inactive_returns_false);
    HU_RUN_TEST(bad_day_build_directive_returns_valid);
    HU_RUN_TEST(behavior_str_returns_expected);
    HU_RUN_TEST(state_deinit_clears_context);
}
