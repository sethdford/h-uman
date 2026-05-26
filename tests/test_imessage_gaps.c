/* tests/test_imessage_gaps.c
 *
 * Tier 1 #3 from docs/plans/2026-05-19-better-than-human.md: conversation
 * gap classifier tests. The pure classifier is fully testable; the SQL
 * scanner returns HU_ERR_NOT_SUPPORTED under HU_IS_TEST so we test the
 * stub contract instead. */

#include "test_framework.h"

#ifdef HU_HAS_IMESSAGE

#include "human/channels/imessage_gaps.h"

#include <stdint.h>

/* Standard thresholds we recommend for production callers — matches
 * the header docstring. */
#define DEFAULT_MIN_HISTORY 10U
#define DEFAULT_MIN_GAP     14
#define DEFAULT_MAX_GAP     365

#define DAY 86400LL

static void test_gap_classify_two_week_silence_is_stale(void) {
    /* Last message 21 days ago, 50 historical → STALE. */
    int64_t now = 1700000000;
    int64_t last = now - 21 * DAY;
    HU_ASSERT_TRUE(hu_imessage_gap_classify_stale(last, now, 50, DEFAULT_MIN_HISTORY,
                                                  DEFAULT_MIN_GAP, DEFAULT_MAX_GAP));
}

static void test_gap_classify_recent_messages_not_stale(void) {
    /* 3 days ago → NOT stale (below min_gap_days). */
    int64_t now = 1700000000;
    int64_t last = now - 3 * DAY;
    HU_ASSERT_TRUE(!hu_imessage_gap_classify_stale(last, now, 50, DEFAULT_MIN_HISTORY,
                                                   DEFAULT_MIN_GAP, DEFAULT_MAX_GAP));
}

static void test_gap_classify_dormant_contact_excluded(void) {
    /* 2 years ago is "dormant," not "stale." Don't surface for outreach
     * — too old to be a "you should check in" signal. */
    int64_t now = 1700000000;
    int64_t last = now - 730 * DAY;
    HU_ASSERT_TRUE(!hu_imessage_gap_classify_stale(last, now, 50, DEFAULT_MIN_HISTORY,
                                                   DEFAULT_MIN_GAP, DEFAULT_MAX_GAP));
}

static void test_gap_classify_one_off_contact_excluded(void) {
    /* Only 5 historical messages — below min_history. Even if the gap
     * matches, we don't flag it. One-off contacts going silent isn't
     * relationship erosion; it's noise. */
    int64_t now = 1700000000;
    int64_t last = now - 30 * DAY;
    HU_ASSERT_TRUE(!hu_imessage_gap_classify_stale(last, now, 5, DEFAULT_MIN_HISTORY,
                                                   DEFAULT_MIN_GAP, DEFAULT_MAX_GAP));
}

static void test_gap_classify_clock_skew_rejected(void) {
    /* Future last_message_unix (clock skew or test fixture bug) → don't
     * classify; better to drop the signal than emit a confusing alert. */
    int64_t now = 1700000000;
    int64_t last_future = now + DAY;
    HU_ASSERT_TRUE(!hu_imessage_gap_classify_stale(last_future, now, 50, DEFAULT_MIN_HISTORY,
                                                   DEFAULT_MIN_GAP, DEFAULT_MAX_GAP));
}

static void test_gap_classify_zero_inputs_safe(void) {
    /* Defensive: zero/negative inputs return false, don't crash. */
    HU_ASSERT_TRUE(!hu_imessage_gap_classify_stale(0, 1700000000, 50, 10, 14, 365));
    HU_ASSERT_TRUE(!hu_imessage_gap_classify_stale(1700000000, 0, 50, 10, 14, 365));
    HU_ASSERT_TRUE(!hu_imessage_gap_classify_stale(-1, 1700000000, 50, 10, 14, 365));
}

static void test_gap_classify_boundary_min_gap(void) {
    /* Exactly min_gap_days → stale (inclusive). One day under → not. */
    int64_t now = 1700000000;
    HU_ASSERT_TRUE(hu_imessage_gap_classify_stale(now - 14 * DAY, now, 50, 10, 14, 365));
    HU_ASSERT_TRUE(!hu_imessage_gap_classify_stale(now - 13 * DAY, now, 50, 10, 14, 365));
}

static void test_gap_classify_boundary_max_gap(void) {
    /* Exactly max_gap_days → stale. One day over → dormant. */
    int64_t now = 1700000000;
    HU_ASSERT_TRUE(hu_imessage_gap_classify_stale(now - 365 * DAY, now, 50, 10, 14, 365));
    HU_ASSERT_TRUE(!hu_imessage_gap_classify_stale(now - 366 * DAY, now, 50, 10, 14, 365));
}

static void test_scan_returns_not_supported_in_test_mode(void) {
    /* The SQL scanner is gated for determinism. Tests pin the stub
     * contract — out_n cleared to 0, return NOT_SUPPORTED — so production
     * callers can react to that signal without inspecting build flags. */
    hu_imessage_stale_contact_t out[4] = {0};
    size_t n = 99;
    hu_error_t err =
        hu_imessage_scan_stale_contacts("/tmp/nonexistent.db", 1700000000, 10, 14, 365, out, 4, &n);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ((int)n, 0);
}

void run_imessage_gaps_tests(void) {
    HU_TEST_SUITE("imessage_gaps");
    HU_RUN_TEST(test_gap_classify_two_week_silence_is_stale);
    HU_RUN_TEST(test_gap_classify_recent_messages_not_stale);
    HU_RUN_TEST(test_gap_classify_dormant_contact_excluded);
    HU_RUN_TEST(test_gap_classify_one_off_contact_excluded);
    HU_RUN_TEST(test_gap_classify_clock_skew_rejected);
    HU_RUN_TEST(test_gap_classify_zero_inputs_safe);
    HU_RUN_TEST(test_gap_classify_boundary_min_gap);
    HU_RUN_TEST(test_gap_classify_boundary_max_gap);
    HU_RUN_TEST(test_scan_returns_not_supported_in_test_mode);
}

#else /* !HU_HAS_IMESSAGE — stub runner so the symbol resolves */

void run_imessage_gaps_tests(void) {
    (void)0;
}

#endif /* HU_HAS_IMESSAGE */
