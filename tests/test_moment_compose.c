#include "human/moment.h"
#include "test_framework.h"

static void compose_from_inputs_rejects_null_out(void) {
    hu_error_t err =
        hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, NULL, 1700000000, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void compose_from_inputs_zero_inputs_produces_safe_defaults(void) {
    hu_moment_t m = {0};
    hu_error_t err = hu_moment_compose_from_inputs(NULL, NULL, NULL, -1, -1, NULL, 1700000000, &m);
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(m.time_since_their_last_msg_s, -1);
    HU_ASSERT_EQ(m.time_since_our_last_msg_s, -1);
    HU_ASSERT_FALSE(m.thread_is_continuation);
    HU_ASSERT_FALSE(m.topic_still_open);
    HU_ASSERT_EQ(m.suggested_open, HU_MOMENT_OPEN_NONE);
    HU_ASSERT_EQ(m.suggested_brevity, HU_MOMENT_BREVITY_MIRROR);
    HU_ASSERT_EQ(m.defer_send_until_s, 0);
    HU_ASSERT_EQ(m.composed_at_s, 1700000000);
    HU_ASSERT_EQ(m.source_flags, 0u);
}

void run_moment_compose_tests(void) {
    HU_TEST_SUITE("moment_compose");
    HU_RUN_TEST(compose_from_inputs_rejects_null_out);
    HU_RUN_TEST(compose_from_inputs_zero_inputs_produces_safe_defaults);
}
