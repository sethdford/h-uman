/* Phase 9 integration tests — F103–F115 end-to-end */
#include "human/context/authentic.h"
#include "human/context/cognitive_load.h"
#include "human/memory.h"
#include "test_framework.h"
#include <string.h>
#include <time.h>
#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

#ifdef HU_ENABLE_SQLITE
#ifdef HU_ENABLE_AUTHENTIC

static void test_cognitive_load_affects_prompt(void) {
    hu_cognitive_load_config_t config = {
        .peak_hour_start = 9,
        .peak_hour_end = 17,
        .low_hour_start = 22,
        .low_hour_end = 6,
        .fatigue_threshold = 5,
        .monday_penalty = 0.1f,
        .friday_bonus = 0.05f,
    };
    /* 2 AM Wed → low capacity → hint non-NULL */
    struct tm tm_2am = {0};
    tm_2am.tm_year = 124;
    tm_2am.tm_mon = 2;
    tm_2am.tm_mday = 13;
    tm_2am.tm_hour = 2;
    tm_2am.tm_min = 0;
    tm_2am.tm_sec = 0;
    tm_2am.tm_isdst = -1;
    time_t t_2am = mktime(&tm_2am);
    HU_ASSERT_TRUE(t_2am != (time_t)-1);
    hu_cognitive_load_state_t state_2am = hu_cognitive_load_calculate(&config, 0, t_2am);
    const char *hint_2am = hu_cognitive_load_prompt_hint(&state_2am);
    HU_ASSERT_NOT_NULL(hint_2am);

    /* 10 AM Wed → peak capacity → hint NULL */
    struct tm tm_10am = {0};
    tm_10am.tm_year = 124;
    tm_10am.tm_mon = 2;
    tm_10am.tm_mday = 13;
    tm_10am.tm_hour = 10;
    tm_10am.tm_min = 0;
    tm_10am.tm_sec = 0;
    tm_10am.tm_isdst = -1;
    time_t t_10am = mktime(&tm_10am);
    HU_ASSERT_TRUE(t_10am != (time_t)-1);
    hu_cognitive_load_state_t state_10am = hu_cognitive_load_calculate(&config, 0, t_10am);
    const char *hint_10am = hu_cognitive_load_prompt_hint(&state_10am);
    HU_ASSERT_NULL(hint_10am);
}

static void test_physical_state_affects_prompt(void) {
    hu_physical_config_t config = {0};
    /* 6 AM → TIRED hint */
    struct tm tm_6am = {0};
    tm_6am.tm_year = 124;
    tm_6am.tm_mon = 2;
    tm_6am.tm_mday = 13;
    tm_6am.tm_hour = 6;
    tm_6am.tm_min = 0;
    tm_6am.tm_sec = 0;
    tm_6am.tm_isdst = -1;
    time_t t_6am = mktime(&tm_6am);
    HU_ASSERT_TRUE(t_6am != (time_t)-1);
    hu_physical_state_t s_6am = hu_physical_state_from_schedule(&config, t_6am);
    const char *hint_6am = hu_physical_state_prompt_hint(s_6am);
    HU_ASSERT_NOT_NULL(hint_6am);
    HU_ASSERT_TRUE(strstr(hint_6am, "Tired") != NULL);

    /* 12 PM → EATING hint */
    struct tm tm_12 = {0};
    tm_12.tm_year = 124;
    tm_12.tm_mon = 2;
    tm_12.tm_mday = 13;
    tm_12.tm_hour = 12;
    tm_12.tm_min = 30;
    tm_12.tm_sec = 0;
    tm_12.tm_isdst = -1;
    time_t t_12 = mktime(&tm_12);
    HU_ASSERT_TRUE(t_12 != (time_t)-1);
    hu_physical_state_t s_12 = hu_physical_state_from_schedule(&config, t_12);
    const char *hint_12 = hu_physical_state_prompt_hint(s_12);
    HU_ASSERT_NOT_NULL(hint_12);
    HU_ASSERT_TRUE(strstr(hint_12, "eating") != NULL);

    /* 3 PM → NORMAL → NULL hint */
    struct tm tm_15 = {0};
    tm_15.tm_year = 124;
    tm_15.tm_mon = 2;
    tm_15.tm_mday = 13;
    tm_15.tm_hour = 15;
    tm_15.tm_min = 0;
    tm_15.tm_sec = 0;
    tm_15.tm_isdst = -1;
    time_t t_15 = mktime(&tm_15);
    HU_ASSERT_TRUE(t_15 != (time_t)-1);
    hu_physical_state_t s_15 = hu_physical_state_from_schedule(&config, t_15);
    const char *hint_15 = hu_physical_state_prompt_hint(s_15);
    HU_ASSERT_NULL(hint_15);
}

static void test_error_injection_rate_statistical(void) {
    int count = 0;
    for (uint32_t seed = 0; seed < 1000u; seed++) {
        if (hu_should_inject_error(0.03f, seed))
            count++;
    }
    /* 3% of 1000 = 30. Allow 20–40 (2–4%) for variance */
    HU_ASSERT_TRUE(count >= 20 && count <= 40);
}

static void test_mundane_complaint_contextual(void) {
    /* Morning weekday = traffic */
    const char *p_morning = hu_mundane_complaint_prompt(8, 2, HU_PHYSICAL_NORMAL, NULL);
    HU_ASSERT_NOT_NULL(p_morning);
    HU_ASSERT_TRUE(strstr(p_morning, "traffic") != NULL || strstr(p_morning, "commute") != NULL);

    /* Lunch = food */
    const char *p_lunch = hu_mundane_complaint_prompt(12, 2, HU_PHYSICAL_NORMAL, NULL);
    HU_ASSERT_NOT_NULL(p_lunch);
    HU_ASSERT_TRUE(strstr(p_lunch, "lunch") != NULL || strstr(p_lunch, "food") != NULL);

    /* Evening = home */
    const char *p_evening = hu_mundane_complaint_prompt(20, 2, HU_PHYSICAL_NORMAL, NULL);
    HU_ASSERT_NOT_NULL(p_evening);
    HU_ASSERT_TRUE(strstr(p_evening, "home") != NULL || strstr(p_evening, "neighbors") != NULL ||
                   strstr(p_evening, "chore") != NULL);
}
static void test_bad_day_recovery_timing(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_memory_t mem = hu_sqlite_memory_create(&alloc, ":memory:");
    sqlite3 *db = hu_sqlite_memory_get_db(&mem);
    HU_ASSERT_NOT_NULL(db);

    int64_t now = (int64_t)time(NULL);
    HU_ASSERT_EQ(hu_interaction_quality_record(db, "contact_b", 0.2f, 0.9f, "frustrated", now),
                 HU_OK);

    /* 1h later: too soon (min_age 2h, max_age 24h) */
    int n_1h = hu_interaction_quality_needs_recovery(db, "contact_b", 0.5f, 2 * 3600, 24 * 3600,
                                                     now + 3600);
    HU_ASSERT_EQ(n_1h, 0);

    /* 4h later: eligible */
    int n_4h = hu_interaction_quality_needs_recovery(db, "contact_b", 0.5f, 2 * 3600, 24 * 3600,
                                                     now + 4 * 3600);
    HU_ASSERT_EQ(n_4h, 1);

    HU_ASSERT_EQ(hu_interaction_quality_mark_recovered(db, "contact_b", now + 5 * 3600), HU_OK);

    int n_after = hu_interaction_quality_needs_recovery(db, "contact_b", 0.5f, 2 * 3600, 24 * 3600,
                                                        now + 6 * 3600);
    HU_ASSERT_EQ(n_after, 0);

    mem.vtable->deinit(mem.ctx);
}
static void test_cognitive_plus_physical_combined(void) {
    hu_cognitive_load_config_t cog_config = {
        .peak_hour_start = 9,
        .peak_hour_end = 17,
        .low_hour_start = 22,
        .low_hour_end = 6,
        .fatigue_threshold = 5,
        .monday_penalty = 0.0f,
        .friday_bonus = 0.0f,
    };
    /* 6 AM Wed: low cognitive + tired physical */
    struct tm tm = {0};
    tm.tm_year = 124;
    tm.tm_mon = 2;
    tm.tm_mday = 13;
    tm.tm_hour = 6;
    tm.tm_min = 0;
    tm.tm_sec = 0;
    tm.tm_isdst = -1;
    time_t t = mktime(&tm);
    HU_ASSERT_TRUE(t != (time_t)-1);

    hu_cognitive_load_state_t cog = hu_cognitive_load_calculate(&cog_config, 0, t);
    hu_physical_config_t phys_config = {0};
    hu_physical_state_t phys = hu_physical_state_from_schedule(&phys_config, t);

    const char *cog_hint = hu_cognitive_load_prompt_hint(&cog);
    const char *phys_hint = hu_physical_state_prompt_hint(phys);

    HU_ASSERT_NOT_NULL(cog_hint);
    HU_ASSERT_NOT_NULL(phys_hint);
}
#endif /* HU_ENABLE_AUTHENTIC */
#endif /* HU_ENABLE_SQLITE */

void run_phase9_integration_tests(void) {
    HU_TEST_SUITE("phase9_integration");
#ifdef HU_ENABLE_SQLITE
#ifdef HU_ENABLE_AUTHENTIC
    HU_RUN_TEST(test_cognitive_load_affects_prompt);
    HU_RUN_TEST(test_physical_state_affects_prompt);
    HU_RUN_TEST(test_error_injection_rate_statistical);
    HU_RUN_TEST(test_mundane_complaint_contextual);
    HU_RUN_TEST(test_bad_day_recovery_timing);
    HU_RUN_TEST(test_cognitive_plus_physical_combined);
#endif
#endif
}
