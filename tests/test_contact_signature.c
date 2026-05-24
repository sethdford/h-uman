/* tests/test_contact_signature.c
 *
 * Per-contact relationship signature tests. The pure helpers (TOD
 * bucketing, median latency, initiation ratio) are fully testable on
 * every build; the SQL-backed `_compute` and `_top_n` paths are gated
 * for determinism so we pin the stub contract instead.
 *
 * Body wrapped in #ifdef HU_HAS_IMESSAGE with stub runner per
 * .claude/rules/test-source-gate-symmetry.md — the underlying source
 * file lives in the HU_HAS_IMESSAGE block, so variants with
 * HU_HAS_IMESSAGE=OFF would fail to link without this wrap. */

#ifdef HU_HAS_IMESSAGE

#include "human/channels/contact_signature.h"
#include "test_framework.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HOUR   3600LL
#define DAY    86400LL
#define MINUTE 60LL

/* --- TOD bucketing --------------------------------------------------- */

static void test_tod_bucket_03_00_is_night(void) {
    /* 03:00 UTC, tz_offset = 0 → NIGHT. */
    int64_t ts = 3 * HOUR;
    HU_ASSERT_EQ((int)hu_tod_bucket_from_unix(ts, 0), (int)HU_TOD_NIGHT);
}

static void test_tod_bucket_09_00_is_morning(void) {
    int64_t ts = 9 * HOUR;
    HU_ASSERT_EQ((int)hu_tod_bucket_from_unix(ts, 0), (int)HU_TOD_MORNING);
}

static void test_tod_bucket_14_00_is_afternoon(void) {
    int64_t ts = 14 * HOUR;
    HU_ASSERT_EQ((int)hu_tod_bucket_from_unix(ts, 0), (int)HU_TOD_AFTERNOON);
}

static void test_tod_bucket_21_00_is_evening(void) {
    int64_t ts = 21 * HOUR;
    HU_ASSERT_EQ((int)hu_tod_bucket_from_unix(ts, 0), (int)HU_TOD_EVENING);
}

static void test_tod_bucket_boundaries_inclusive_at_start(void) {
    /* Bucket boundaries: 00:00 = NIGHT, 06:00 = MORNING, 12:00 = AFTERNOON,
     * 18:00 = EVENING. Each is INCLUSIVE on its lower edge. */
    HU_ASSERT_EQ((int)hu_tod_bucket_from_unix(0 * HOUR, 0), (int)HU_TOD_NIGHT);
    HU_ASSERT_EQ((int)hu_tod_bucket_from_unix(6 * HOUR, 0), (int)HU_TOD_MORNING);
    HU_ASSERT_EQ((int)hu_tod_bucket_from_unix(12 * HOUR, 0), (int)HU_TOD_AFTERNOON);
    HU_ASSERT_EQ((int)hu_tod_bucket_from_unix(18 * HOUR, 0), (int)HU_TOD_EVENING);
    /* 24:00 wraps back to NIGHT. */
    HU_ASSERT_EQ((int)hu_tod_bucket_from_unix(24 * HOUR, 0), (int)HU_TOD_NIGHT);
}

static void test_tod_bucket_tz_offset_shifts_window(void) {
    /* 02:00 UTC with PST (-28800) → 18:00 local previous day → EVENING. */
    int64_t utc_0200 = 2 * HOUR;
    HU_ASSERT_EQ((int)hu_tod_bucket_from_unix(utc_0200, -28800), (int)HU_TOD_EVENING);
    /* 10:00 UTC with PST → 02:00 local → NIGHT. */
    HU_ASSERT_EQ((int)hu_tod_bucket_from_unix(10 * HOUR, -28800), (int)HU_TOD_NIGHT);
}

static void test_tod_bucket_negative_ts_safe(void) {
    /* Pre-1970 input: shouldn't crash, should pick a deterministic
     * bucket. Just exercise the math path. */
    int bucket = (int)hu_tod_bucket_from_unix(-1, 0);
    HU_ASSERT_TRUE(bucket >= 0 && bucket < (int)HU_TOD_BUCKET_COUNT);
}

/* --- Median latency -------------------------------------------------- */

static void test_median_latency_empty_returns_negative_one(void) {
    HU_ASSERT_EQ((int)hu_signature_median_latency(NULL, 0), -1);
    int32_t empty[1] = {0};
    HU_ASSERT_EQ((int)hu_signature_median_latency(empty, 0), -1);
}

static void test_median_latency_single_value(void) {
    int32_t lats[] = {42};
    HU_ASSERT_EQ((int)hu_signature_median_latency(lats, 1), 42);
}

static void test_median_latency_odd_count_returns_middle(void) {
    int32_t lats[] = {1, 5, 100};
    HU_ASSERT_EQ((int)hu_signature_median_latency(lats, 3), 5);
}

static void test_median_latency_even_count_averages_middle_two(void) {
    int32_t lats[] = {10, 20, 30, 40};
    /* Middle two are 20 and 30 → average 25. */
    HU_ASSERT_EQ((int)hu_signature_median_latency(lats, 4), 25);
}

/* --- Initiation ratio ------------------------------------------------ */

static void test_initiation_ratio_empty_neutral(void) {
    HU_ASSERT_TRUE(hu_signature_initiation_ratio(NULL, NULL, 0, 0) == 0.5);
}

static void test_initiation_ratio_all_outbound_is_one(void) {
    /* Three messages, all from me, 5 hours apart → 3 conversations,
     * all initiated by me → 1.0. */
    int64_t ts[] = {0, 5 * HOUR, 10 * HOUR};
    bool fm[] = {true, true, true};
    double r = hu_signature_initiation_ratio(ts, fm, 3, 4 * HOUR);
    HU_ASSERT_TRUE(r == 1.0);
}

static void test_initiation_ratio_all_inbound_is_zero(void) {
    int64_t ts[] = {0, 5 * HOUR, 10 * HOUR};
    bool fm[] = {false, false, false};
    double r = hu_signature_initiation_ratio(ts, fm, 3, 4 * HOUR);
    HU_ASSERT_TRUE(r == 0.0);
}

static void test_initiation_ratio_alternating_initiators_balanced(void) {
    /* Two conversations 5 hours apart. First initiated by me, second by
     * them → 1/2 = 0.5. */
    int64_t ts[] = {0, 1 * MINUTE, 5 * HOUR, 5 * HOUR + 1 * MINUTE};
    bool fm[] = {true, false, false, true};
    double r = hu_signature_initiation_ratio(ts, fm, 4, 4 * HOUR);
    HU_ASSERT_TRUE(r == 0.5);
}

static void test_initiation_ratio_gap_threshold_splits_conversations(void) {
    /* All within 1 hour → single conversation, initiated by me → 1.0.
     * Same data with a much-smaller threshold splits into separate
     * conversations. */
    int64_t ts[] = {0, 30 * MINUTE, 45 * MINUTE};
    bool fm[] = {true, false, true};
    /* Threshold 4h → single conversation, initiated by me → 1.0. */
    HU_ASSERT_TRUE(hu_signature_initiation_ratio(ts, fm, 3, 4 * HOUR) == 1.0);
    /* Threshold 10 min → 3 conversations, my-initiated = 2 (msgs 0 + 2)
     * → 2/3. */
    double r2 = hu_signature_initiation_ratio(ts, fm, 3, 10 * 60);
    HU_ASSERT_TRUE(r2 > 0.66 && r2 < 0.67);
}

/* --- SQL stub contract ----------------------------------------------- */

static void test_compute_returns_not_supported_in_test_mode(void) {
    hu_contact_signature_t sig;
    /* Pre-fill so we can verify it gets reset rather than left dirty. */
    sig.total_messages = 99;
    hu_error_t err = hu_contact_signature_compute("/tmp/no.db", "+15551234567", 1700000000, &sig);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ((int)sig.total_messages, 0);
}

static void test_top_n_returns_not_supported_in_test_mode(void) {
    hu_contact_signature_t sigs[4];
    size_t n = 99;
    hu_error_t err = hu_contact_signature_top_n("/tmp/no.db", 1700000000, 4, sigs, 4, &n);
    HU_ASSERT_EQ((int)err, (int)HU_ERR_NOT_SUPPORTED);
    HU_ASSERT_EQ((int)n, 0);
}

/* --- Adversarial: aggregation against the pure helpers --------------- */

static void test_initiation_ratio_single_message_neutral(void) {
    /* One message can't establish a pattern. Returns the ratio that
     * arises naturally from a single conversation: 1.0 if from me,
     * 0.0 if from them. The contract is "no crash + sane number". */
    int64_t ts[] = {1700000000};
    bool fm_me[] = {true};
    bool fm_them[] = {false};
    HU_ASSERT_TRUE(hu_signature_initiation_ratio(ts, fm_me, 1, 4 * HOUR) == 1.0);
    HU_ASSERT_TRUE(hu_signature_initiation_ratio(ts, fm_them, 1, 4 * HOUR) == 0.0);
}

static void test_initiation_ratio_zero_threshold_uses_default(void) {
    /* Threshold <= 0 should fall back to the 4-hour default. Two
     * messages 1 hour apart → single conversation, my-initiated. */
    int64_t ts[] = {0, 1 * HOUR};
    bool fm[] = {true, false};
    HU_ASSERT_TRUE(hu_signature_initiation_ratio(ts, fm, 2, 0) == 1.0);
    HU_ASSERT_TRUE(hu_signature_initiation_ratio(ts, fm, 2, -1) == 1.0);
}

void run_contact_signature_tests(void) {
    HU_TEST_SUITE("contact_signature");
    HU_RUN_TEST(test_tod_bucket_03_00_is_night);
    HU_RUN_TEST(test_tod_bucket_09_00_is_morning);
    HU_RUN_TEST(test_tod_bucket_14_00_is_afternoon);
    HU_RUN_TEST(test_tod_bucket_21_00_is_evening);
    HU_RUN_TEST(test_tod_bucket_boundaries_inclusive_at_start);
    HU_RUN_TEST(test_tod_bucket_tz_offset_shifts_window);
    HU_RUN_TEST(test_tod_bucket_negative_ts_safe);
    HU_RUN_TEST(test_median_latency_empty_returns_negative_one);
    HU_RUN_TEST(test_median_latency_single_value);
    HU_RUN_TEST(test_median_latency_odd_count_returns_middle);
    HU_RUN_TEST(test_median_latency_even_count_averages_middle_two);
    HU_RUN_TEST(test_initiation_ratio_empty_neutral);
    HU_RUN_TEST(test_initiation_ratio_all_outbound_is_one);
    HU_RUN_TEST(test_initiation_ratio_all_inbound_is_zero);
    HU_RUN_TEST(test_initiation_ratio_alternating_initiators_balanced);
    HU_RUN_TEST(test_initiation_ratio_gap_threshold_splits_conversations);
    HU_RUN_TEST(test_compute_returns_not_supported_in_test_mode);
    HU_RUN_TEST(test_top_n_returns_not_supported_in_test_mode);
    HU_RUN_TEST(test_initiation_ratio_single_message_neutral);
    HU_RUN_TEST(test_initiation_ratio_zero_threshold_uses_default);
}

#else /* HU_HAS_IMESSAGE not defined — provide stub so symbol resolves */

void run_contact_signature_tests(void) {
    (void)0;
}

#endif /* HU_HAS_IMESSAGE */
