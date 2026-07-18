#include "human/behavior/tapback_band.h"
#include "human/core/allocator.h"
#include "test_framework.h"

#include <stdio.h>
#include <string.h>

#define MIN_MS (60LL * 1000LL)

static hu_tapback_band_t band_with_p90(int64_t p90_ms) {
    hu_tapback_band_t b = {0};
    b.valid = true;
    b.rate = 0.08;
    b.p50_ms = p90_ms / 4;
    b.p90_ms = p90_ms;
    return b;
}

/* ── hu_tapback_within_band truth table ─────────────────────────────── */

static void tapback_fresh_no_band_within_default_cap(void) {
    /* 30s old, no bands file at all → within the 15-min default cap. */
    HU_ASSERT_TRUE(hu_tapback_within_band(1000LL * MIN_MS, 1000LL * MIN_MS - 30 * 1000, NULL));
}

static void tapback_stale_no_band_dropped(void) {
    /* The 2026-07-18 audit case: ~100 min late → outside the default cap. */
    HU_ASSERT_FALSE(hu_tapback_within_band(1000LL * MIN_MS, 1000LL * MIN_MS - 100 * MIN_MS, NULL));
}

static void tapback_invalid_band_uses_default_cap(void) {
    hu_tapback_band_t b = {0}; /* valid=false — missing-bands default */
    HU_ASSERT_TRUE(hu_tapback_within_band(1000LL * MIN_MS, 1000LL * MIN_MS - 14 * MIN_MS, &b));
    HU_ASSERT_FALSE(hu_tapback_within_band(1000LL * MIN_MS, 1000LL * MIN_MS - 16 * MIN_MS, &b));
}

static void tapback_valid_band_p90_is_the_cap(void) {
    hu_tapback_band_t b = band_with_p90(2 * MIN_MS);
    HU_ASSERT_TRUE(hu_tapback_within_band(1000LL * MIN_MS, 1000LL * MIN_MS - 90 * 1000, &b));
    HU_ASSERT_FALSE(hu_tapback_within_band(1000LL * MIN_MS, 1000LL * MIN_MS - 3 * MIN_MS, &b));
}

static void tapback_valid_band_zero_p90_falls_back_to_default(void) {
    hu_tapback_band_t b = band_with_p90(0);
    HU_ASSERT_TRUE(hu_tapback_within_band(1000LL * MIN_MS, 1000LL * MIN_MS - 10 * MIN_MS, &b));
    HU_ASSERT_FALSE(hu_tapback_within_band(1000LL * MIN_MS, 1000LL * MIN_MS - 20 * MIN_MS, &b));
}

static void tapback_unknown_origin_within_band(void) {
    /* timestamp 0/negative = origin unknown → do not drop on missing data. */
    HU_ASSERT_TRUE(hu_tapback_within_band(1000LL * MIN_MS, 0, NULL));
    HU_ASSERT_TRUE(hu_tapback_within_band(1000LL * MIN_MS, -5, NULL));
}

static void tapback_negative_age_within_band(void) {
    /* Clock skew: message "from the future" is not stale. */
    HU_ASSERT_TRUE(hu_tapback_within_band(1000LL * MIN_MS, 1001LL * MIN_MS, NULL));
}

static void tapback_boundary_age_equals_cap_within_band(void) {
    hu_tapback_band_t b = band_with_p90(2 * MIN_MS);
    HU_ASSERT_TRUE(hu_tapback_within_band(1000LL * MIN_MS, 1000LL * MIN_MS - 2 * MIN_MS, &b));
    HU_ASSERT_FALSE(hu_tapback_within_band(1000LL * MIN_MS, 1000LL * MIN_MS - 2 * MIN_MS - 1, &b));
}

static void tapback_age_form_matches_timestamp_form(void) {
    /* age-based form: same cap semantics as the timestamp form. */
    hu_tapback_band_t b = band_with_p90(2 * MIN_MS);
    HU_ASSERT_TRUE(hu_tapback_age_within_band(90 * 1000, &b));
    HU_ASSERT_FALSE(hu_tapback_age_within_band(3 * MIN_MS, &b));
    /* no band → 15-min default cap */
    HU_ASSERT_TRUE(hu_tapback_age_within_band(10 * MIN_MS, NULL));
    HU_ASSERT_FALSE(hu_tapback_age_within_band(100 * MIN_MS, NULL));
    /* unknown / negative age → within band */
    HU_ASSERT_TRUE(hu_tapback_age_within_band(0, NULL));
    HU_ASSERT_TRUE(hu_tapback_age_within_band(-30, NULL));
}

/* ── dispatch wrapper (the symbol the daemon send path calls) ───────── */

static void tapback_dispatch_stale_msg_drops(void) {
    /* Director chose tapback for a message received 100 min ago, no bands
     * file → the dispatch gate must say DROP, never send late. */
    int64_t now_sec = 1752854400;
    HU_ASSERT_FALSE(hu_tapback_dispatch_within_band(now_sec, now_sec - 100 * 60, NULL));
}

static void tapback_dispatch_fresh_msg_sends(void) {
    int64_t now_sec = 1752854400;
    HU_ASSERT_TRUE(hu_tapback_dispatch_within_band(now_sec, now_sec - 30, NULL));
}

static void tapback_dispatch_unknown_ts_sends(void) {
    /* timestamp_sec == 0 means "use poll time" per hu_channel_loop_msg_t. */
    HU_ASSERT_TRUE(hu_tapback_dispatch_within_band(1752854400, 0, NULL));
}

static void tapback_dispatch_band_tightens_cap(void) {
    hu_tapback_band_t b = band_with_p90(2 * MIN_MS);
    int64_t now_sec = 1752854400;
    HU_ASSERT_FALSE(hu_tapback_dispatch_within_band(now_sec, now_sec - 5 * 60, &b));
    HU_ASSERT_TRUE(hu_tapback_dispatch_within_band(now_sec, now_sec - 60, &b));
}

/* ── bands JSON parse / load ────────────────────────────────────────── */

static const char *BANDS_JSON = "{\n"
                                "  \"generated_at\": \"2026-07-18\",\n"
                                "  \"default\": {\"rate\": 0.06, \"p50_ms\": 45000, "
                                "\"p90_ms\": 600000},\n"
                                "  \"contacts\": {\n"
                                "    \"+15555550123\": {\"rate\": 0.10, \"n\": 42, "
                                "\"p50_ms\": 32000, \"p90_ms\": 120000}\n"
                                "  }\n"
                                "}\n";

static void tapback_parse_contact_band(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tapback_band_t b;
    HU_ASSERT_EQ(hu_tapback_band_parse(&alloc, BANDS_JSON, strlen(BANDS_JSON), "+15555550123", &b),
                 HU_OK);
    HU_ASSERT_TRUE(b.valid);
    HU_ASSERT_EQ((int)b.p90_ms, 120000);
    HU_ASSERT_EQ((int)b.p50_ms, 32000);
}

static void tapback_parse_unknown_contact_uses_default_band(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tapback_band_t b;
    HU_ASSERT_EQ(hu_tapback_band_parse(&alloc, BANDS_JSON, strlen(BANDS_JSON), "+19998887777", &b),
                 HU_OK);
    HU_ASSERT_TRUE(b.valid);
    HU_ASSERT_EQ((int)b.p90_ms, 600000);
}

static void tapback_parse_no_bands_yields_invalid(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{\"generated_at\": \"2026-07-18\"}";
    hu_tapback_band_t b;
    HU_ASSERT_EQ(hu_tapback_band_parse(&alloc, json, strlen(json), "+15555550123", &b), HU_OK);
    HU_ASSERT_FALSE(b.valid);
}

static void tapback_parse_malformed_json_invalid_band(void) {
    hu_allocator_t alloc = hu_system_allocator();
    const char *json = "{not json";
    hu_tapback_band_t b;
    hu_error_t err = hu_tapback_band_parse(&alloc, json, strlen(json), "+15555550123", &b);
    HU_ASSERT_TRUE(err != HU_OK);
    HU_ASSERT_FALSE(b.valid);
}

static void tapback_load_missing_file_invalid_band_default_cap(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_tapback_band_t b;
    hu_error_t err =
        hu_tapback_band_load(&alloc, "/nonexistent/tapback_bands.json", "+15555550123", &b);
    HU_ASSERT_TRUE(err != HU_OK);
    HU_ASSERT_FALSE(b.valid);
    /* Missing bands file → predicate falls back to the 15-min default cap. */
    HU_ASSERT_TRUE(hu_tapback_within_band(1000LL * MIN_MS, 1000LL * MIN_MS - 10 * MIN_MS, &b));
    HU_ASSERT_FALSE(hu_tapback_within_band(1000LL * MIN_MS, 1000LL * MIN_MS - 100 * MIN_MS, &b));
}

static void tapback_default_path_null_in_tests(void) {
    char buf[512];
    HU_ASSERT_TRUE(hu_tapback_bands_default_path(buf, sizeof(buf)) == NULL);
}

void run_tapback_band_tests(void) {
    HU_TEST_SUITE("TapbackBand");
    HU_RUN_TEST(tapback_fresh_no_band_within_default_cap);
    HU_RUN_TEST(tapback_stale_no_band_dropped);
    HU_RUN_TEST(tapback_invalid_band_uses_default_cap);
    HU_RUN_TEST(tapback_valid_band_p90_is_the_cap);
    HU_RUN_TEST(tapback_valid_band_zero_p90_falls_back_to_default);
    HU_RUN_TEST(tapback_unknown_origin_within_band);
    HU_RUN_TEST(tapback_negative_age_within_band);
    HU_RUN_TEST(tapback_boundary_age_equals_cap_within_band);
    HU_RUN_TEST(tapback_age_form_matches_timestamp_form);
    HU_RUN_TEST(tapback_dispatch_stale_msg_drops);
    HU_RUN_TEST(tapback_dispatch_fresh_msg_sends);
    HU_RUN_TEST(tapback_dispatch_unknown_ts_sends);
    HU_RUN_TEST(tapback_dispatch_band_tightens_cap);
    HU_RUN_TEST(tapback_parse_contact_band);
    HU_RUN_TEST(tapback_parse_unknown_contact_uses_default_band);
    HU_RUN_TEST(tapback_parse_no_bands_yields_invalid);
    HU_RUN_TEST(tapback_parse_malformed_json_invalid_band);
    HU_RUN_TEST(tapback_load_missing_file_invalid_band_default_cap);
    HU_RUN_TEST(tapback_default_path_null_in_tests);
}
