#include "human/context/authentic.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>
#ifdef HU_ENABLE_SQLITE
#include "human/memory.h"
#include <sqlite3.h>
#endif

static void select_zero_probabilities_returns_none(void) {
    hu_authentic_config_t config = {0};
    hu_authentic_behavior_t b = hu_authentic_select(&config, 0.9, false, 12345u);
    HU_ASSERT_EQ(b, HU_AUTH_NONE);
}

static void select_high_closeness_serious_topic_suppresses_gossip(void) {
    hu_authentic_config_t config = {0};
    config.narration_probability = 0.0;
    config.gossip_probability = 1.0;
    config.random_thought_probability = 0.0;
    config.imperfection_probability = 0.0;
    /* With serious_topic, gossip/random_thought/imperfection are suppressed */
    hu_authentic_behavior_t b = hu_authentic_select(&config, 0.9, true, 999u);
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
    hu_authentic_behavior_t b = hu_authentic_select(&config, 0.3, false, 111u);
    HU_ASSERT_NEQ(b, HU_AUTH_EXISTENTIAL);
    HU_ASSERT_NEQ(b, HU_AUTH_GUILT);
    HU_ASSERT_NEQ(b, HU_AUTH_EMBODIMENT);
}

static void select_resistance_only_high_closeness(void) {
    hu_authentic_config_t config = {0};
    config.resistance_probability = 1.0;
    hu_authentic_behavior_t b_low = hu_authentic_select(&config, 0.5, false, 1u);
    hu_authentic_behavior_t b_high = hu_authentic_select(&config, 0.8, false, 1u);
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
    hu_error_t err =
        hu_authentic_build_directive(&alloc, HU_AUTH_NARRATION, NULL, 0, &out, &out_len);
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
    hu_error_t err =
        hu_authentic_build_directive(&alloc, HU_AUTH_EMBODIMENT, NULL, 0, &out, &out_len);
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
    hu_error_t err =
        hu_authentic_build_directive(&alloc, HU_AUTH_LIFE_THREAD, ctx, strlen(ctx), &out, &out_len);
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
    hu_error_t err =
        hu_authentic_build_directive(&alloc, HU_AUTH_LIFE_THREAD, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "narrative") != NULL);
    hu_str_free(&alloc, out);
}

static void build_directive_none_returns_ok(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 1;
    hu_error_t err = hu_authentic_build_directive(&alloc, HU_AUTH_NONE, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NULL(out);
    HU_ASSERT_EQ(out_len, 0u);
}

static void build_directive_bad_day_returns_valid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_authentic_build_directive(&alloc, HU_AUTH_BAD_DAY, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "rough day") != NULL);
    hu_str_free(&alloc, out);
}
static void build_directive_null_alloc_returns_error(void) {
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_authentic_build_directive(NULL, HU_AUTH_NARRATION, NULL, 0, &out, &out_len);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* F104/F105: Physical state and imperfection (Tasks 3 and 4) */
static void test_physical_state_morning_tired(void) {
    struct tm tm_buf = {0};
    tm_buf.tm_year = 124;
    tm_buf.tm_mon = 0;
    tm_buf.tm_mday = 15;
    tm_buf.tm_hour = 6;
    tm_buf.tm_min = 0;
    tm_buf.tm_sec = 0;
    tm_buf.tm_isdst = -1;
    time_t now = mktime(&tm_buf);
    hu_physical_config_t config = {0};
    hu_physical_state_t s = hu_physical_state_from_schedule(&config, now);
    HU_ASSERT_EQ(s, HU_PHYSICAL_TIRED);
}

static void test_physical_state_post_coffee(void) {
    struct tm tm_buf = {0};
    tm_buf.tm_year = 124;
    tm_buf.tm_mon = 0;
    tm_buf.tm_mday = 15;
    tm_buf.tm_hour = 8;
    tm_buf.tm_min = 0;
    tm_buf.tm_sec = 0;
    tm_buf.tm_isdst = -1;
    time_t now = mktime(&tm_buf);
    hu_physical_config_t config = {0};
    config.coffee_drinker = true;
    hu_physical_state_t s = hu_physical_state_from_schedule(&config, now);
    HU_ASSERT_EQ(s, HU_PHYSICAL_CAFFEINATED);
}

static void test_physical_state_lunch(void) {
    struct tm tm_buf = {0};
    tm_buf.tm_year = 124;
    tm_buf.tm_mon = 0;
    tm_buf.tm_mday = 15;
    tm_buf.tm_hour = 12;
    tm_buf.tm_min = 30;
    tm_buf.tm_sec = 0;
    tm_buf.tm_isdst = -1;
    time_t now = mktime(&tm_buf);
    hu_physical_config_t config = {0};
    hu_physical_state_t s = hu_physical_state_from_schedule(&config, now);
    HU_ASSERT_EQ(s, HU_PHYSICAL_EATING);
}

static void test_physical_state_late_night(void) {
    struct tm tm_buf = {0};
    tm_buf.tm_year = 124;
    tm_buf.tm_mon = 0;
    tm_buf.tm_mday = 15;
    tm_buf.tm_hour = 23;
    tm_buf.tm_min = 0;
    tm_buf.tm_sec = 0;
    tm_buf.tm_isdst = -1;
    time_t now = mktime(&tm_buf);
    hu_physical_config_t config = {0};
    hu_physical_state_t s = hu_physical_state_from_schedule(&config, now);
    HU_ASSERT_EQ(s, HU_PHYSICAL_TIRED);
}

static void test_physical_state_post_gym(void) {
    /* Jan 1 2024 is Monday (tm_wday=1) */
    struct tm tm_buf = {0};
    tm_buf.tm_year = 124;
    tm_buf.tm_mon = 0;
    tm_buf.tm_mday = 1;
    tm_buf.tm_hour = 17;
    tm_buf.tm_min = 0;
    tm_buf.tm_sec = 0;
    tm_buf.tm_isdst = -1;
    time_t now = mktime(&tm_buf);
    hu_physical_config_t config = {0};
    config.exercises = true;
    config.exercise_days[0] = 1; /* Monday */
    config.exercise_day_count = 1;
    hu_physical_state_t s = hu_physical_state_from_schedule(&config, now);
    HU_ASSERT_EQ(s, HU_PHYSICAL_SORE);
}

static void test_physical_state_name(void) {
    HU_ASSERT_STR_EQ(hu_physical_state_name(HU_PHYSICAL_NORMAL), "normal");
    HU_ASSERT_STR_EQ(hu_physical_state_name(HU_PHYSICAL_TIRED), "tired");
    HU_ASSERT_STR_EQ(hu_physical_state_name(HU_PHYSICAL_CAFFEINATED), "caffeinated");
    HU_ASSERT_STR_EQ(hu_physical_state_name(HU_PHYSICAL_EATING), "eating");
    HU_ASSERT_STR_EQ(hu_physical_state_name(HU_PHYSICAL_SORE), "sore");
}

static void test_physical_state_prompt_hint_normal(void) {
    const char *hint = hu_physical_state_prompt_hint(HU_PHYSICAL_NORMAL);
    HU_ASSERT_NULL(hint);
}

static void test_physical_state_prompt_hint_tired(void) {
    const char *hint = hu_physical_state_prompt_hint(HU_PHYSICAL_TIRED);
    HU_ASSERT_NOT_NULL(hint);
    HU_ASSERT_TRUE(strstr(hint, "Tired") != NULL);
}

static void test_error_injection_probability(void) {
    int count = 0;
    for (uint32_t seed = 0; seed < 10000u; seed++) {
        if (hu_should_inject_error(0.03f, seed))
            count++;
    }
    /* 3% of 10000 = 300. Allow 200-400 for variance. */
    HU_ASSERT_TRUE(count >= 200 && count <= 400);
}

static void test_error_injection_prompt_content(void) {
    const char *p = hu_error_injection_prompt();
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_TRUE(strstr(p, "misremember") != NULL);
    HU_ASSERT_TRUE(strstr(p, "NEVER be wrong") != NULL);
}

/* F106: Mundane complaining */
static void test_mundane_complaint_weekday_morning(void) {
    const char *p = hu_mundane_complaint_prompt(8, 2, HU_PHYSICAL_NORMAL, NULL);
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_TRUE(strstr(p, "traffic") != NULL || strstr(p, "commute") != NULL);
}

static void test_mundane_complaint_hot_weather(void) {
    const char *p = hu_mundane_complaint_prompt(10, 3, HU_PHYSICAL_NORMAL, "hot");
    HU_ASSERT_NOT_NULL(p);
    HU_ASSERT_TRUE(strstr(p, "hot") != NULL);
}

static void test_mundane_complaint_weekend_morning(void) {
    const char *p = hu_mundane_complaint_prompt(8, 0, HU_PHYSICAL_NORMAL, NULL);
    HU_ASSERT_NULL(p);
}

#ifdef HU_ENABLE_SQLITE
static void test_quality_record_needs_recovery(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t now = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_interaction_quality_record(db, "contact_z", 0.2f, 0.9f, "frustrated", now),
                 HU_OK);

    int n = hu_interaction_quality_needs_recovery(db, "contact_z", 0.5f, 0, 86400, now + 1);
    HU_ASSERT_EQ(n, 1);

    mem.vtable->deinit(mem.ctx);
}

static void test_quality_mark_recovered(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t now = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_interaction_quality_record(db, "contact_w", 0.3f, 0.8f, "tired", now), HU_OK);

    int n = hu_interaction_quality_needs_recovery(db, "contact_w", 0.5f, 0, 86400, now + 1);
    HU_ASSERT_EQ(n, 1);

    HU_ASSERT_EQ(hu_interaction_quality_mark_recovered(db, "contact_w", now + 2), HU_OK);

    n = hu_interaction_quality_needs_recovery(db, "contact_w", 0.5f, 0, 86400, now + 3);
    HU_ASSERT_EQ(n, 0);

    mem.vtable->deinit(mem.ctx);
}
#endif /* HU_ENABLE_SQLITE */

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
#ifdef HU_ENABLE_SQLITE
#endif
    HU_RUN_TEST(test_physical_state_morning_tired);
    HU_RUN_TEST(test_physical_state_post_coffee);
    HU_RUN_TEST(test_physical_state_lunch);
    HU_RUN_TEST(test_physical_state_late_night);
    HU_RUN_TEST(test_physical_state_post_gym);
    HU_RUN_TEST(test_physical_state_name);
    HU_RUN_TEST(test_physical_state_prompt_hint_normal);
    HU_RUN_TEST(test_physical_state_prompt_hint_tired);
    HU_RUN_TEST(test_error_injection_probability);
    HU_RUN_TEST(test_error_injection_prompt_content);
    HU_RUN_TEST(test_mundane_complaint_weekday_morning);
    HU_RUN_TEST(test_mundane_complaint_hot_weather);
    HU_RUN_TEST(test_mundane_complaint_weekend_morning);
#ifdef HU_ENABLE_SQLITE
    HU_RUN_TEST(test_quality_record_needs_recovery);
    HU_RUN_TEST(test_quality_mark_recovered);
#endif
}
