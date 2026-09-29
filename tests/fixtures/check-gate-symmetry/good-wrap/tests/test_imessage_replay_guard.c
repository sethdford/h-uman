/* Fixture: the POST-FIX shape of tests/test_imessage_replay_guard.c (012ced741).
 * Whole body wrapped in the platform macro, stub runner keeps the symbol. */
#include "test_framework.h"

#if HU_HAS_IMESSAGE

#include "human/channels/imessage.h"

static void test_resume_no_persisted_seeds_from_db_max(void) {
    int64_t skipped = -1;
    HU_ASSERT_EQ(hu_imessage_resume_rowid(0, 70491, 50, &skipped), 70491);
}

static void test_stale_beyond_window(void) {
    HU_ASSERT_TRUE(hu_imessage_inbound_is_stale(1788309259 - 86401, 1788309259, 86400));
}

void run_imessage_replay_guard_tests(void) {
    HU_TEST_SUITE("imessage_replay_guard");
    HU_RUN_TEST(test_resume_no_persisted_seeds_from_db_max);
    HU_RUN_TEST(test_stale_beyond_window);
}

#else /* !HU_HAS_IMESSAGE */

void run_imessage_replay_guard_tests(void) {
    (void)0;
}

#endif /* HU_HAS_IMESSAGE */
