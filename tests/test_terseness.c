/* test_terseness.c — terseness calibration gate (OFF/SHADOW/LIVE) + directive. */
#include "human/persona/terseness.h"
#include "test_framework.h"
#include <string.h>

static void terse_parse_off_when_unset(void) {
    HU_ASSERT_EQ((int)hu_terse_mode_parse(NULL, false, false), (int)HU_TERSE_OFF);
    HU_ASSERT_EQ((int)hu_terse_mode_parse("off", false, false), (int)HU_TERSE_OFF);
    HU_ASSERT_EQ((int)hu_terse_mode_parse("0", false, false), (int)HU_TERSE_OFF);
}

static void terse_parse_shadow(void) {
    HU_ASSERT_EQ((int)hu_terse_mode_parse("shadow", false, false), (int)HU_TERSE_SHADOW);
    HU_ASSERT_EQ((int)hu_terse_mode_parse("1", false, false), (int)HU_TERSE_SHADOW);
    HU_ASSERT_EQ((int)hu_terse_mode_parse(NULL, false, true), (int)HU_TERSE_SHADOW);
}

static void terse_parse_live(void) {
    HU_ASSERT_EQ((int)hu_terse_mode_parse("live", false, false), (int)HU_TERSE_LIVE);
    HU_ASSERT_EQ((int)hu_terse_mode_parse("2", false, false), (int)HU_TERSE_LIVE);
    HU_ASSERT_EQ((int)hu_terse_mode_parse(NULL, true, false), (int)HU_TERSE_LIVE);
}

static void terse_parse_live_beats_shadow(void) {
    /* Precedence LIVE > SHADOW > OFF. */
    HU_ASSERT_EQ((int)hu_terse_mode_parse("live", false, true), (int)HU_TERSE_LIVE);
    HU_ASSERT_EQ((int)hu_terse_mode_parse(NULL, true, true), (int)HU_TERSE_LIVE);
}

static void terse_directive_targets_the_measured_gap(void) {
    const char *d = hu_terse_directive();
    HU_ASSERT_NOT_NULL(d);
    /* Must push terseness AND suppress the chirpy closers that inflate the
     * "endearing/polished" blind-A/B signal. */
    HU_ASSERT(strstr(d, "terse") != NULL || strstr(d, "short") != NULL);
    HU_ASSERT(strstr(d, "sign-off") != NULL || strstr(d, "well-wish") != NULL ||
              strstr(d, "drive safe") != NULL);
    /* The directive itself must be plain text (no markdown leaking into prompt). */
    HU_ASSERT(strstr(d, "**") == NULL);
    HU_ASSERT(strchr(d, '>') == NULL);
}

void run_terseness_tests(void) {
    HU_TEST_SUITE("terseness");
    HU_RUN_TEST(terse_parse_off_when_unset);
    HU_RUN_TEST(terse_parse_shadow);
    HU_RUN_TEST(terse_parse_live);
    HU_RUN_TEST(terse_parse_live_beats_shadow);
    HU_RUN_TEST(terse_directive_targets_the_measured_gap);
}
