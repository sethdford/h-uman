#include "human/context/cognitive_load.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>

/* Tue 10 AM = peak hours → capacity near 1.0 */
static void test_peak_hours_capacity_near_one(void) {
    hu_cognitive_load_config_t config = {
        .peak_hour_start = 9,
        .peak_hour_end = 17,
        .low_hour_start = 22,
        .low_hour_end = 6,
        .fatigue_threshold = 5,
        .monday_penalty = 0.1f,
        .friday_bonus = 0.05f,
    };
    /* Tue 10 AM: 2024-03-12 10:00:00 */
    struct tm tm = {0};
    tm.tm_year = 124;
    tm.tm_mon = 2;
    tm.tm_mday = 12;
    tm.tm_hour = 10;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    HU_ASSERT_TRUE(t != (time_t)-1);

    hu_cognitive_load_state_t state = hu_cognitive_load_calculate(&config, 0, t);
    HU_ASSERT_TRUE(state.capacity >= 0.95f);
    HU_ASSERT_TRUE(state.capacity <= 1.0f);
    HU_ASSERT_EQ(state.hour_of_day, 10);
    HU_ASSERT_EQ(state.day_of_week, 2); /* Tuesday */
}

/* Wed 2 AM = low hours → capacity near 0.3 */
static void test_low_hours_capacity_near_point_three(void) {
    hu_cognitive_load_config_t config = {
        .peak_hour_start = 9,
        .peak_hour_end = 17,
        .low_hour_start = 22,
        .low_hour_end = 6,
        .fatigue_threshold = 5,
        .monday_penalty = 0.1f,
        .friday_bonus = 0.05f,
    };
    /* Wed 2 AM: 2024-03-13 02:00:00 */
    struct tm tm = {0};
    tm.tm_year = 124;
    tm.tm_mon = 2;
    tm.tm_mday = 13;
    tm.tm_hour = 2;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    HU_ASSERT_TRUE(t != (time_t)-1);

    hu_cognitive_load_state_t state = hu_cognitive_load_calculate(&config, 0, t);
    HU_ASSERT_TRUE(state.capacity >= 0.25f);
    HU_ASSERT_TRUE(state.capacity <= 0.35f);
}

/* Monday penalty → lower than Tuesday at same hour */
static void test_monday_penalty_lower_than_tuesday(void) {
    hu_cognitive_load_config_t config = {
        .peak_hour_start = 9,
        .peak_hour_end = 17,
        .low_hour_start = 22,
        .low_hour_end = 6,
        .fatigue_threshold = 5,
        .monday_penalty = 0.15f,
        .friday_bonus = 0.0f,
    };
    /* Mon 10 AM */
    struct tm tm_mon = {0};
    tm_mon.tm_year = 124;
    tm_mon.tm_mon = 2;
    tm_mon.tm_mday = 11;
    tm_mon.tm_hour = 10;
    tm_mon.tm_min = 0;
    tm_mon.tm_sec = 0;
    tm_mon.tm_isdst = -1;
    time_t t_mon = mktime(&tm_mon);
    HU_ASSERT_TRUE(t_mon != (time_t)-1);

    /* Tue 10 AM */
    struct tm tm_tue = {0};
    tm_tue.tm_year = 124;
    tm_tue.tm_mon = 2;
    tm_tue.tm_mday = 12;
    tm_tue.tm_hour = 10;
    tm_tue.tm_min = 0;
    tm_tue.tm_sec = 0;
    tm_tue.tm_isdst = -1;
    time_t t_tue = mktime(&tm_tue);
    HU_ASSERT_TRUE(t_tue != (time_t)-1);

    hu_cognitive_load_state_t state_mon = hu_cognitive_load_calculate(&config, 0, t_mon);
    hu_cognitive_load_state_t state_tue = hu_cognitive_load_calculate(&config, 0, t_tue);
    HU_ASSERT_TRUE(state_mon.capacity < state_tue.capacity);
}

/* Friday bonus → higher than Thursday at same hour */
static void test_friday_bonus_higher_than_thursday(void) {
    hu_cognitive_load_config_t config = {
        .peak_hour_start = 9,
        .peak_hour_end = 17,
        .low_hour_start = 22,
        .low_hour_end = 6,
        .fatigue_threshold = 5,
        .monday_penalty = 0.0f,
        .friday_bonus = 0.1f,
    };
    /* Thu 8 AM (transition hour, base < 1.0 so Friday bonus is visible) */
    struct tm tm_thu = {0};
    tm_thu.tm_year = 124;
    tm_thu.tm_mon = 2;
    tm_thu.tm_mday = 14;
    tm_thu.tm_hour = 8;
    tm_thu.tm_min = 0;
    tm_thu.tm_sec = 0;
    tm_thu.tm_isdst = -1;
    time_t t_thu = mktime(&tm_thu);
    HU_ASSERT_TRUE(t_thu != (time_t)-1);

    /* Fri 8 AM */
    struct tm tm_fri = {0};
    tm_fri.tm_year = 124;
    tm_fri.tm_mon = 2;
    tm_fri.tm_mday = 15;
    tm_fri.tm_hour = 8;
    tm_fri.tm_min = 0;
    tm_fri.tm_sec = 0;
    tm_fri.tm_isdst = -1;
    time_t t_fri = mktime(&tm_fri);
    HU_ASSERT_TRUE(t_fri != (time_t)-1);

    hu_cognitive_load_state_t state_thu = hu_cognitive_load_calculate(&config, 0, t_thu);
    hu_cognitive_load_state_t state_fri = hu_cognitive_load_calculate(&config, 0, t_fri);
    HU_ASSERT_TRUE(state_fri.capacity > state_thu.capacity);
}

/* Conversation fatigue → depth 0 vs depth 20 decreasing */
static void test_conversation_fatigue_decreases_capacity(void) {
    hu_cognitive_load_config_t config = {
        .peak_hour_start = 9,
        .peak_hour_end = 17,
        .low_hour_start = 22,
        .low_hour_end = 6,
        .fatigue_threshold = 5,
        .monday_penalty = 0.0f,
        .friday_bonus = 0.0f,
    };
    /* Tue 10 AM */
    struct tm tm = {0};
    tm.tm_year = 124;
    tm.tm_mon = 2;
    tm.tm_mday = 12;
    tm.tm_hour = 10;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    HU_ASSERT_TRUE(t != (time_t)-1);

    hu_cognitive_load_state_t state_0 = hu_cognitive_load_calculate(&config, 0, t);
    hu_cognitive_load_state_t state_20 = hu_cognitive_load_calculate(&config, 20, t);
    HU_ASSERT_TRUE(state_20.capacity < state_0.capacity);
    /* 15 exchanges beyond threshold * 0.05 = 0.75 reduction */
    HU_ASSERT_TRUE(state_20.capacity < 0.5f);
}

/* Prompt hint peak → NULL */
static void test_prompt_hint_peak_returns_null(void) {
    hu_cognitive_load_state_t state = {.capacity = 0.95f};
    const char *hint = hu_cognitive_load_prompt_hint(&state);
    HU_ASSERT_NULL(hint);
}

/* Prompt hint tired → non-NULL */
static void test_prompt_hint_tired_returns_non_null(void) {
    hu_cognitive_load_state_t state = {.capacity = 0.6f};
    const char *hint = hu_cognitive_load_prompt_hint(&state);
    HU_ASSERT_NOT_NULL(hint);
    HU_ASSERT_TRUE(strstr(hint, "Slightly tired") != NULL);
}

/* Max response length → scales with capacity */
static void test_max_response_length_scales_with_capacity(void) {
    hu_cognitive_load_state_t state_high = {.capacity = 1.0f};
    hu_cognitive_load_state_t state_low = {.capacity = 0.2f};
    int len_high = hu_cognitive_load_max_response_length(&state_high);
    int len_low = hu_cognitive_load_max_response_length(&state_low);
    HU_ASSERT_TRUE(len_high > len_low);
    HU_ASSERT_TRUE(len_high >= 195 && len_high <= 220);
    HU_ASSERT_TRUE(len_low >= 35 && len_low <= 90);
}

void run_cognitive_load_tests(void) {
    HU_TEST_SUITE("cognitive_load");
    HU_RUN_TEST(test_peak_hours_capacity_near_one);
    HU_RUN_TEST(test_low_hours_capacity_near_point_three);
    HU_RUN_TEST(test_monday_penalty_lower_than_tuesday);
    HU_RUN_TEST(test_friday_bonus_higher_than_thursday);
    HU_RUN_TEST(test_conversation_fatigue_decreases_capacity);
    HU_RUN_TEST(test_prompt_hint_peak_returns_null);
    HU_RUN_TEST(test_prompt_hint_tired_returns_non_null);
    HU_RUN_TEST(test_max_response_length_scales_with_capacity);
}
