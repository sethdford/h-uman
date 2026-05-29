/* Characterization tests for the proactive check-in policy leaves
 * (DDD Phase 2b scaffold). Pins the exact behavior extracted from
 * hu_service_run_proactive_checkins so future decomposition can't drift it. */
#include "human/daemon/proactive_policy.h"
#include "test_framework.h"
#include <string.h>

/* Social-hours window is 09:00–21:00 inclusive: pin both edges + outside. */
static void social_hour_window_is_9_to_21_inclusive(void) {
    /* Below the window. */
    HU_ASSERT_FALSE(hu_daemon_proactive_is_social_hour(0));
    HU_ASSERT_FALSE(hu_daemon_proactive_is_social_hour(8));
    /* Inclusive lower edge. */
    HU_ASSERT_TRUE(hu_daemon_proactive_is_social_hour(9));
    /* Midday. */
    HU_ASSERT_TRUE(hu_daemon_proactive_is_social_hour(12));
    /* Inclusive upper edge. */
    HU_ASSERT_TRUE(hu_daemon_proactive_is_social_hour(21));
    /* Above the window. */
    HU_ASSERT_FALSE(hu_daemon_proactive_is_social_hour(22));
    HU_ASSERT_FALSE(hu_daemon_proactive_is_social_hour(23));
}

/* YMD packing matches the original (year+1900)*10000+(mon+1)*100+mday. */
static void ymd_from_tm_packs_yyyymmdd(void) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = 2026 - 1900; /* tm_year is years since 1900 */
    t.tm_mon = 4;            /* tm_mon is 0-based: 4 = May */
    t.tm_mday = 29;
    HU_ASSERT_EQ(hu_daemon_proactive_ymd_from_tm(&t), 20260529);

    /* Single-digit month/day zero-pad into their fields. */
    memset(&t, 0, sizeof(t));
    t.tm_year = 2000 - 1900;
    t.tm_mon = 0; /* January */
    t.tm_mday = 1;
    HU_ASSERT_EQ(hu_daemon_proactive_ymd_from_tm(&t), 20000101);
}

/* Defensive: NULL tm yields 0 (the production call site never passes NULL). */
static void ymd_from_tm_null_is_zero(void) {
    HU_ASSERT_EQ(hu_daemon_proactive_ymd_from_tm(NULL), 0);
}

void run_proactive_policy_tests(void) {
    HU_TEST_SUITE("proactive_policy");
    HU_RUN_TEST(social_hour_window_is_9_to_21_inclusive);
    HU_RUN_TEST(ymd_from_tm_packs_yyyymmdd);
    HU_RUN_TEST(ymd_from_tm_null_is_zero);
}
