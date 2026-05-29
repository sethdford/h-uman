/* ─────────────────────────────────────────────────────────────────────────
 * test_prosocial_routine.c — pins C-series scheduler + B0-gated prompt builder.
 * Spec: docs/plans/2026-05-29-prosocial-uplift/
 * ───────────────────────────────────────────────────────────────────────── */

#include "human/agent/prosocial_routine.h"
#include "human/behavior/prosocial.h"
#include "human/core/allocator.h"
#include "test_framework.h"

/* A "nothing recent, mid-morning, weekday" baseline; tests perturb fields. */
static hu_routine_facts_t base(void) {
    hu_routine_facts_t f;
    f.local_hour = 8;  /* in morning window */
    f.day_of_week = 3; /* Wednesday */
    f.user_active = false;
    f.secs_since_morning = 100 * 3600;
    f.secs_since_evening = 100 * 3600;
    f.secs_since_weekly = 100 * 24 * 3600;
    f.secs_since_thinking = 100 * 24 * 3600;
    return f;
}

static void routine_user_active_yields_none(void) {
    hu_routine_facts_t f = base();
    f.user_active = true;
    HU_ASSERT_EQ((int)hu_routine_due(&f), (int)HU_ROUTINE_NONE);
}

static void routine_morning_fires(void) {
    hu_routine_facts_t f = base();
    f.local_hour = 8;
    HU_ASSERT_EQ((int)hu_routine_due(&f), (int)HU_ROUTINE_MORNING_INTENTION);
}

static void routine_evening_fires(void) {
    hu_routine_facts_t f = base();
    f.local_hour = 21;
    HU_ASSERT_EQ((int)hu_routine_due(&f), (int)HU_ROUTINE_EVENING_REFLECTION);
}

static void routine_weekly_fires_sunday(void) {
    hu_routine_facts_t f = base();
    f.day_of_week = 0; /* Sunday */
    f.local_hour = 11; /* weekly window [10,14) */
    HU_ASSERT_EQ((int)hu_routine_due(&f), (int)HU_ROUTINE_WEEKLY_CHECKIN);
}

static void routine_thinking_fires_on_long_gap(void) {
    hu_routine_facts_t f = base();
    f.local_hour = 15; /* outside morning/evening; daytime */
    HU_ASSERT_EQ((int)hu_routine_due(&f), (int)HU_ROUTINE_THINKING_OF_YOU);
}

static void routine_recent_gap_suppresses(void) {
    hu_routine_facts_t f = base();
    f.local_hour = 8;
    f.secs_since_morning = 60; /* just ran */
    HU_ASSERT_EQ((int)hu_routine_due(&f), (int)HU_ROUTINE_NONE);
}

static void routine_offhours_none(void) {
    hu_routine_facts_t f = base();
    f.local_hour = 3; /* nothing scheduled at 3am */
    f.secs_since_thinking = 0;
    HU_ASSERT_EQ((int)hu_routine_due(&f), (int)HU_ROUTINE_NONE);
}

static void routine_null_none(void) {
    HU_ASSERT_EQ((int)hu_routine_due(NULL), (int)HU_ROUTINE_NONE);
}

/* ── Builder (B0-gated) ─────────────────────────────────────────────────── */

static void routine_builder_each_kind_honest(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_routine_kind_t kinds[] = {HU_ROUTINE_MORNING_INTENTION, HU_ROUTINE_EVENING_REFLECTION,
                                 HU_ROUTINE_WEEKLY_CHECKIN, HU_ROUTINE_THINKING_OF_YOU};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        size_t len = 0;
        char *p = hu_routine_build_prompt(&alloc, kinds[i], HU_BRISK_NONE, &len);
        HU_ASSERT_NOT_NULL(p);
        HU_ASSERT_TRUE(len > 0);
        HU_ASSERT_FALSE(hu_prosocial_text_claims_feeling(p, len));
        alloc.free(alloc.ctx, p, len + 1);
    }
}

static void routine_builder_suppressed_on_dependency(void) {
    hu_allocator_t alloc = hu_system_allocator();
    size_t len = 99;
    char *p = hu_routine_build_prompt(&alloc, HU_ROUTINE_MORNING_INTENTION,
                                      HU_BRISK_DEPENDENCY_PATTERN, &len);
    HU_ASSERT_NULL(p);
    HU_ASSERT_EQ(len, 0u);
}

void run_prosocial_routine_tests(void);
void run_prosocial_routine_tests(void) {
    HU_TEST_SUITE("prosocial_routine");
    HU_RUN_TEST(routine_user_active_yields_none);
    HU_RUN_TEST(routine_morning_fires);
    HU_RUN_TEST(routine_evening_fires);
    HU_RUN_TEST(routine_weekly_fires_sunday);
    HU_RUN_TEST(routine_thinking_fires_on_long_gap);
    HU_RUN_TEST(routine_recent_gap_suppresses);
    HU_RUN_TEST(routine_offhours_none);
    HU_RUN_TEST(routine_null_none);
    HU_RUN_TEST(routine_builder_each_kind_honest);
    HU_RUN_TEST(routine_builder_suppressed_on_dependency);
}
