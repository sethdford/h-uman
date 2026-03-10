#include "human/core/allocator.h"
#include "human/persona/circadian.h"
#include "test_framework.h"
#include <string.h>

static void circadian_5am_is_early_morning(void) {
    hu_time_phase_t p = hu_circadian_phase(5);
    HU_ASSERT_EQ(p, HU_PHASE_EARLY_MORNING);
}

static void circadian_10am_is_morning(void) {
    hu_time_phase_t p = hu_circadian_phase(10);
    HU_ASSERT_EQ(p, HU_PHASE_MORNING);
}

static void circadian_14pm_is_afternoon(void) {
    hu_time_phase_t p = hu_circadian_phase(14);
    HU_ASSERT_EQ(p, HU_PHASE_AFTERNOON);
}

static void circadian_19pm_is_evening(void) {
    hu_time_phase_t p = hu_circadian_phase(19);
    HU_ASSERT_EQ(p, HU_PHASE_EVENING);
}

static void circadian_22pm_is_night(void) {
    hu_time_phase_t p = hu_circadian_phase(22);
    HU_ASSERT_EQ(p, HU_PHASE_NIGHT);
}

static void circadian_2am_is_late_night(void) {
    hu_time_phase_t p = hu_circadian_phase(2);
    HU_ASSERT_EQ(p, HU_PHASE_LATE_NIGHT);
}

static void circadian_build_prompt_contains_phase(void) {
    hu_allocator_t alloc = hu_system_allocator();
    char *out = NULL;
    size_t out_len = 0;
    hu_error_t err = hu_circadian_build_prompt(&alloc, 10, &out, &out_len);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_NOT_NULL(out);
    HU_ASSERT_TRUE(strstr(out, "Time Awareness") != NULL);
    HU_ASSERT_TRUE(strstr(out, "morning") != NULL);
    alloc.free(alloc.ctx, out, out_len + 1);
}

static void circadian_boundary_hours(void) {
    HU_ASSERT_EQ(hu_circadian_phase(0), HU_PHASE_LATE_NIGHT);
    HU_ASSERT_EQ(hu_circadian_phase(5), HU_PHASE_EARLY_MORNING);
    HU_ASSERT_EQ(hu_circadian_phase(8), HU_PHASE_MORNING);
    HU_ASSERT_EQ(hu_circadian_phase(12), HU_PHASE_AFTERNOON);
    HU_ASSERT_EQ(hu_circadian_phase(17), HU_PHASE_EVENING);
    HU_ASSERT_EQ(hu_circadian_phase(21), HU_PHASE_NIGHT);
}

void run_circadian_tests(void) {
    HU_TEST_SUITE("circadian");
    HU_RUN_TEST(circadian_5am_is_early_morning);
    HU_RUN_TEST(circadian_10am_is_morning);
    HU_RUN_TEST(circadian_14pm_is_afternoon);
    HU_RUN_TEST(circadian_19pm_is_evening);
    HU_RUN_TEST(circadian_22pm_is_night);
    HU_RUN_TEST(circadian_2am_is_late_night);
    HU_RUN_TEST(circadian_build_prompt_contains_phase);
    HU_RUN_TEST(circadian_boundary_hours);
}
