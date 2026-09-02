/* Fixture: the PRE-FIX shape of tests/test_imessage_replay_guard.c (012ced741^).
 * hu_imessage_resume_rowid / hu_imessage_inbound_is_stale live in
 * src/channels/imessage.c, which is only compiled under HU_HAS_IMESSAGE.
 * Nothing here wraps the calls, so Linux variants fail to link. */
#include "human/channels/imessage.h"
#include "test_framework.h"

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
