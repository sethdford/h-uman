/* Regression guard for the iMessage reactive-reply "whole-message fallback"
 * decision (src/daemon.c send loop). Pins the truth table of the extracted
 * predicate hu_daemon_should_send_whole_reply_fallback so the duplicate-reply
 * bug — where a choreography-planned reply (sent as N segments) was ALSO
 * re-sent in full via the fallback branch — can never silently return.
 *
 * The predicate lives in human/daemon/common.h and is the exact decision used
 * at the `} else if (...)` site in daemon.c, so this test exercises the real
 * production branch logic, not a reimplementation. */
#include "human/daemon/common.h"
#include "test_framework.h"

/* Bug case: choreography fired (sent N segments) and the splitter therefore
 * left frag_count at 0. The whole-message fallback must NOT fire — sending it
 * would duplicate the reply (the "Alexis" double-reply). */
static void whole_fallback_choreography_no_frags_returns_false(void) {
    HU_ASSERT_FALSE(hu_daemon_should_send_whole_reply_fallback(true, 0));
}

/* Choreography fired with a non-zero frag_count (defensive): still must not
 * re-send the whole reply. */
static void whole_fallback_choreography_with_frags_returns_false(void) {
    HU_ASSERT_FALSE(hu_daemon_should_send_whole_reply_fallback(true, 2));
}

/* Legitimate fallback: neither choreography nor the fragment splitter produced
 * any output, so the whole reply must be sent exactly once. */
static void whole_fallback_no_choreography_no_frags_returns_true(void) {
    HU_ASSERT_TRUE(hu_daemon_should_send_whole_reply_fallback(false, 0));
}

/* Fragment splitter already delivered K fragments — the whole-message fallback
 * must NOT also fire. */
static void whole_fallback_no_choreography_with_frags_returns_false(void) {
    HU_ASSERT_FALSE(hu_daemon_should_send_whole_reply_fallback(false, 3));
}

void run_daemon_reply_fallback_tests(void) {
    HU_TEST_SUITE("daemon_reply_fallback");
    HU_RUN_TEST(whole_fallback_choreography_no_frags_returns_false);
    HU_RUN_TEST(whole_fallback_choreography_with_frags_returns_false);
    HU_RUN_TEST(whole_fallback_no_choreography_no_frags_returns_true);
    HU_RUN_TEST(whole_fallback_no_choreography_with_frags_returns_false);
}
