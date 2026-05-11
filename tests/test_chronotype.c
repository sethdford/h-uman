#include "human/persona/circadian.h"
#include "test_framework.h"

#include <string.h>

static void chrono_lark_active_band(void) {
    HU_ASSERT_TRUE(hu_chronotype_is_active_hour(HU_CHRONO_MORNING_LARK, 6));
    HU_ASSERT_TRUE(hu_chronotype_is_active_hour(HU_CHRONO_MORNING_LARK, 21));
    HU_ASSERT_FALSE(hu_chronotype_is_active_hour(HU_CHRONO_MORNING_LARK, 5));
    HU_ASSERT_FALSE(hu_chronotype_is_active_hour(HU_CHRONO_MORNING_LARK, 22));
}

static void chrono_intermediate_default_band(void) {
    HU_ASSERT_TRUE(hu_chronotype_is_active_hour(HU_CHRONO_INTERMEDIATE, 7));
    HU_ASSERT_TRUE(hu_chronotype_is_active_hour(HU_CHRONO_INTERMEDIATE, 22));
    HU_ASSERT_FALSE(hu_chronotype_is_active_hour(HU_CHRONO_INTERMEDIATE, 6));
    HU_ASSERT_FALSE(hu_chronotype_is_active_hour(HU_CHRONO_INTERMEDIATE, 23));
}

static void chrono_owl_includes_late_night(void) {
    HU_ASSERT_TRUE(hu_chronotype_is_active_hour(HU_CHRONO_EVENING_OWL, 9));
    HU_ASSERT_TRUE(hu_chronotype_is_active_hour(HU_CHRONO_EVENING_OWL, 23));
    HU_ASSERT_TRUE(hu_chronotype_is_active_hour(HU_CHRONO_EVENING_OWL, 0));
    HU_ASSERT_TRUE(hu_chronotype_is_active_hour(HU_CHRONO_EVENING_OWL, 1));
    HU_ASSERT_FALSE(hu_chronotype_is_active_hour(HU_CHRONO_EVENING_OWL, 4));
    HU_ASSERT_FALSE(hu_chronotype_is_active_hour(HU_CHRONO_EVENING_OWL, 8));
}

static void chrono_unknown_falls_back_to_intermediate(void) {
    for (uint8_t h = 0; h < 24; h++) {
        int unk = hu_chronotype_is_active_hour(HU_CHRONO_UNKNOWN, h);
        int inter = hu_chronotype_is_active_hour(HU_CHRONO_INTERMEDIATE, h);
        HU_ASSERT_EQ(unk, inter);
    }
}

static void chrono_out_of_range_clamps(void) {
    /* Hour 250 should clamp to 23, which is outside lark/intermediate band
     * but inside owl. */
    HU_ASSERT_FALSE(hu_chronotype_is_active_hour(HU_CHRONO_INTERMEDIATE, 250));
    HU_ASSERT_TRUE(hu_chronotype_is_active_hour(HU_CHRONO_EVENING_OWL, 250));
}

static void chrono_name_known(void) {
    HU_ASSERT_STR_EQ(hu_chronotype_name(HU_CHRONO_MORNING_LARK), "morning_lark");
    HU_ASSERT_STR_EQ(hu_chronotype_name(HU_CHRONO_EVENING_OWL), "evening_owl");
    HU_ASSERT_STR_EQ(hu_chronotype_name(HU_CHRONO_INTERMEDIATE), "intermediate");
    HU_ASSERT_STR_EQ(hu_chronotype_name(HU_CHRONO_UNKNOWN), "unknown");
}

void run_chronotype_tests(void);

void run_chronotype_tests(void) {
    HU_TEST_SUITE("chronotype");
    HU_RUN_TEST(chrono_lark_active_band);
    HU_RUN_TEST(chrono_intermediate_default_band);
    HU_RUN_TEST(chrono_owl_includes_late_night);
    HU_RUN_TEST(chrono_unknown_falls_back_to_intermediate);
    HU_RUN_TEST(chrono_out_of_range_clamps);
    HU_RUN_TEST(chrono_name_known);
}
