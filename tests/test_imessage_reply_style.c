// @covers-none — tests enum constants in header-only construct (hu_reply_style_t)
#include "human/channels/imessage_action.h"
#include "test_framework.h"

static void enum_values_are_stable(void) {
    HU_ASSERT_EQ((int)HU_REPLY_STYLE_FLAT, 0);
    HU_ASSERT_EQ((int)HU_REPLY_STYLE_THREADED, 1);
    HU_ASSERT_EQ((int)HU_REPLY_STYLE_TAPBACK, 2);
    HU_ASSERT_EQ((int)HU_REPLY_STYLE_TAPBACK_PLUS_FLAT, 3);
}

void run_imessage_reply_style_tests(void) {
    HU_TEST_SUITE("imessage_reply_style");
    HU_RUN_TEST(enum_values_are_stable);
}
