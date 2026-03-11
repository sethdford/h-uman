#include "human/core/allocator.h"
#include "human/persona/life_sim.h"
#include "test_framework.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static void life_sim_09_30_weekday_work_meetings_slow(void) {
    /* Routine: 09:00 work_meetings (slow), 12:00 lunch (available).
     * At 09:30 weekday -> activity "work_meetings", availability "slow". */
    hu_daily_routine_t routine = {0};
    routine.routine_variance = 0.15f;
    routine.weekday_count = 2;
    snprintf(routine.weekday[0].time, sizeof(routine.weekday[0].time), "09:00");
    snprintf(routine.weekday[0].activity, sizeof(routine.weekday[0].activity), "work_meetings");
    snprintf(routine.weekday[0].availability, sizeof(routine.weekday[0].availability), "slow");
    snprintf(routine.weekday[0].mood_modifier, sizeof(routine.weekday[0].mood_modifier), "focused");
    snprintf(routine.weekday[1].time, sizeof(routine.weekday[1].time), "12:00");
    snprintf(routine.weekday[1].activity, sizeof(routine.weekday[1].activity), "lunch");
    snprintf(routine.weekday[1].availability, sizeof(routine.weekday[1].availability), "available");
    snprintf(routine.weekday[1].mood_modifier, sizeof(routine.weekday[1].mood_modifier), "relaxed");

    /* Build timestamp for Wednesday 09:30 local. Use a fixed date. */
    struct tm tm = {0};
    tm.tm_year = 124; /* 2024 */
    tm.tm_mon = 2;    /* March */
    tm.tm_mday = 13;  /* Wednesday */
    tm.tm_hour = 9;
    tm.tm_min = 30;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    int64_t now_ts = (int64_t)t;
    int day_of_week = tm.tm_wday; /* 3 = Wednesday */

    hu_life_sim_state_t state = hu_life_sim_get_current(&routine, now_ts, day_of_week, 0);
    HU_ASSERT_STR_EQ(state.activity, "work_meetings");
    HU_ASSERT_STR_EQ(state.availability, "slow");
    HU_ASSERT_FLOAT_EQ(state.availability_factor, 2.0f, 0.001f);
}

static void life_sim_12_30_weekday_lunch_available(void) {
    hu_daily_routine_t routine = {0};
    routine.routine_variance = 0.15f;
    routine.weekday_count = 2;
    snprintf(routine.weekday[0].time, sizeof(routine.weekday[0].time), "09:00");
    snprintf(routine.weekday[0].activity, sizeof(routine.weekday[0].activity), "work_meetings");
    snprintf(routine.weekday[0].availability, sizeof(routine.weekday[0].availability), "slow");
    snprintf(routine.weekday[0].mood_modifier, sizeof(routine.weekday[0].mood_modifier), "focused");
    snprintf(routine.weekday[1].time, sizeof(routine.weekday[1].time), "12:00");
    snprintf(routine.weekday[1].activity, sizeof(routine.weekday[1].activity), "lunch");
    snprintf(routine.weekday[1].availability, sizeof(routine.weekday[1].availability), "available");
    snprintf(routine.weekday[1].mood_modifier, sizeof(routine.weekday[1].mood_modifier), "relaxed");

    struct tm tm = {0};
    tm.tm_year = 124;
    tm.tm_mon = 2;
    tm.tm_mday = 13;
    tm.tm_hour = 12;
    tm.tm_min = 30;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    int64_t now_ts = (int64_t)t;
    int day_of_week = tm.tm_wday;

    hu_life_sim_state_t state = hu_life_sim_get_current(&routine, now_ts, day_of_week, 0);
    HU_ASSERT_STR_EQ(state.activity, "lunch");
    HU_ASSERT_STR_EQ(state.availability, "available");
    HU_ASSERT_FLOAT_EQ(state.availability_factor, 0.5f, 0.001f);
}

static void life_sim_no_routine_defaults(void) {
    hu_life_sim_state_t state = hu_life_sim_get_current(NULL, (int64_t)time(NULL), 3, 0);
    HU_ASSERT_STR_EQ(state.activity, "idle");
    HU_ASSERT_STR_EQ(state.availability, "available");
    HU_ASSERT_FLOAT_EQ(state.availability_factor, 0.5f, 0.001f);
}

static void life_sim_empty_weekday_uses_defaults(void) {
    hu_daily_routine_t routine = {0};
    routine.routine_variance = 0.15f;
    routine.weekday_count = 0;
    routine.weekend_count = 0;
    hu_life_sim_state_t state = hu_life_sim_get_current(&routine, (int64_t)time(NULL), 3, 0);
    HU_ASSERT_STR_EQ(state.activity, "idle");
    HU_ASSERT_STR_EQ(state.availability, "available");
}

static void life_sim_build_context_format(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_life_sim_state_t state = {
        .activity = "work_meetings",
        .availability = "slow",
        .mood_modifier = "focused",
        .availability_factor = 2.0f,
    };
    size_t out_len = 0;
    char *ctx = hu_life_sim_build_context(&alloc, &state, &out_len);
    HU_ASSERT_NOT_NULL(ctx);
    HU_ASSERT_TRUE(strstr(ctx, "LIFE CONTEXT") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "work_meetings") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "slow") != NULL);
    HU_ASSERT_TRUE(strstr(ctx, "focused") != NULL);
    alloc.free(alloc.ctx, ctx, out_len + 1);
}

static void life_sim_weekend_uses_weekend_blocks(void) {
    hu_daily_routine_t routine = {0};
    routine.routine_variance = 0.15f;
    routine.weekday_count = 1;
    snprintf(routine.weekday[0].time, sizeof(routine.weekday[0].time), "09:00");
    snprintf(routine.weekday[0].activity, sizeof(routine.weekday[0].activity), "work");
    snprintf(routine.weekday[0].availability, sizeof(routine.weekday[0].availability), "slow");
    snprintf(routine.weekday[0].mood_modifier, sizeof(routine.weekday[0].mood_modifier), "focused");
    routine.weekend_count = 1;
    snprintf(routine.weekend[0].time, sizeof(routine.weekend[0].time), "09:00");
    snprintf(routine.weekend[0].activity, sizeof(routine.weekend[0].activity), "sleep_in");
    snprintf(routine.weekend[0].availability, sizeof(routine.weekend[0].availability), "available");
    snprintf(routine.weekend[0].mood_modifier, sizeof(routine.weekend[0].mood_modifier), "relaxed");

    struct tm tm = {0};
    tm.tm_year = 124;
    tm.tm_mon = 2;
    tm.tm_mday = 16; /* Saturday */
    tm.tm_hour = 10;
    tm.tm_min = 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    int64_t now_ts = (int64_t)t;
    int day_of_week = tm.tm_wday; /* 6 = Saturday */

    hu_life_sim_state_t state = hu_life_sim_get_current(&routine, now_ts, day_of_week, 0);
    HU_ASSERT_STR_EQ(state.activity, "sleep_in");
    HU_ASSERT_STR_EQ(state.availability, "available");
}

void run_life_sim_tests(void) {
    HU_TEST_SUITE("life_sim");
    HU_RUN_TEST(life_sim_09_30_weekday_work_meetings_slow);
    HU_RUN_TEST(life_sim_12_30_weekday_lunch_available);
    HU_RUN_TEST(life_sim_no_routine_defaults);
    HU_RUN_TEST(life_sim_empty_weekday_uses_defaults);
    HU_RUN_TEST(life_sim_build_context_format);
    HU_RUN_TEST(life_sim_weekend_uses_weekend_blocks);
}
