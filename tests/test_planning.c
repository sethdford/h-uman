#include "human/agent/planning.h"
#include "human/core/allocator.h"
#include "human/core/string.h"
#include "test_framework.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
static void planning_status_str_roundtrip(void) {
    hu_plan_status_t statuses[] = {
        HU_PLAN_PROPOSED,  HU_PLAN_ACCEPTED,  HU_PLAN_CONFIRMED,
        HU_PLAN_COMPLETED, HU_PLAN_CANCELLED, HU_PLAN_DECLINED,
    };
    for (size_t i = 0; i < sizeof(statuses) / sizeof(statuses[0]); i++) {
        const char *str = hu_plan_status_str(statuses[i]);
        hu_plan_status_t out;
        HU_ASSERT_TRUE(hu_plan_status_from_str(str, &out));
        HU_ASSERT_EQ(out, statuses[i]);
    }
}

static void planning_status_from_str_unknown(void) {
    hu_plan_status_t out;
    HU_ASSERT_FALSE(hu_plan_status_from_str("garbage", &out));
    HU_ASSERT_FALSE(hu_plan_status_from_str("", &out));
    HU_ASSERT_FALSE(hu_plan_status_from_str(NULL, &out));
}
void run_planning_tests(void) {
    HU_TEST_SUITE("planning");
    HU_RUN_TEST(planning_status_str_roundtrip);
    HU_RUN_TEST(planning_status_from_str_unknown);
}
