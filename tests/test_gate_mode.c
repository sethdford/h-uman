/* tests/test_gate_mode.c
 *
 * Truth table for hu_gate_mode_parse — the canonical off/shadow/live
 * parser that unifies the per-gate copies (2026-07-12 review). The
 * table pins the SUPERSET contract: every spelling each legacy gate
 * accepted must map to the same state here, unknown input fails closed,
 * and the unset default is the caller's.
 */

#include "human/core/gate_mode.h"
#include "test_framework.h"

#include <stdlib.h>

static void gate_parse_unset_and_empty_take_callers_default(void) {
    HU_ASSERT_EQ((int)hu_gate_mode_parse(NULL, HU_GATE_OFF), (int)HU_GATE_OFF);
    HU_ASSERT_EQ((int)hu_gate_mode_parse(NULL, HU_GATE_SHADOW), (int)HU_GATE_SHADOW);
    HU_ASSERT_EQ((int)hu_gate_mode_parse(NULL, HU_GATE_LIVE), (int)HU_GATE_LIVE);
    HU_ASSERT_EQ((int)hu_gate_mode_parse("", HU_GATE_SHADOW), (int)HU_GATE_SHADOW);
}

static void gate_parse_off_and_shadow(void) {
    HU_ASSERT_EQ((int)hu_gate_mode_parse("off", HU_GATE_SHADOW), (int)HU_GATE_OFF);
    HU_ASSERT_EQ((int)hu_gate_mode_parse("shadow", HU_GATE_OFF), (int)HU_GATE_SHADOW);
}

static void gate_parse_live_aliases(void) {
    HU_ASSERT_EQ((int)hu_gate_mode_parse("on", HU_GATE_OFF), (int)HU_GATE_LIVE);
    HU_ASSERT_EQ((int)hu_gate_mode_parse("live", HU_GATE_OFF), (int)HU_GATE_LIVE);
    HU_ASSERT_EQ((int)hu_gate_mode_parse("1", HU_GATE_OFF), (int)HU_GATE_LIVE);
}

static void gate_parse_is_case_insensitive(void) {
    HU_ASSERT_EQ((int)hu_gate_mode_parse("LIVE", HU_GATE_OFF), (int)HU_GATE_LIVE);
    HU_ASSERT_EQ((int)hu_gate_mode_parse("Shadow", HU_GATE_OFF), (int)HU_GATE_SHADOW);
    HU_ASSERT_EQ((int)hu_gate_mode_parse("OFF", HU_GATE_SHADOW), (int)HU_GATE_OFF);
}

static void gate_parse_unknown_fails_closed_regardless_of_default(void) {
    /* unknown input must never activate behavior — not even the caller's
     * default (a typo in a default-SHADOW gate must not silently shadow) */
    HU_ASSERT_EQ((int)hu_gate_mode_parse("bogus", HU_GATE_SHADOW), (int)HU_GATE_OFF);
    HU_ASSERT_EQ((int)hu_gate_mode_parse("LIVEish", HU_GATE_LIVE), (int)HU_GATE_OFF);
    HU_ASSERT_EQ((int)hu_gate_mode_parse("2", HU_GATE_LIVE), (int)HU_GATE_OFF);
}

static void gate_from_env_reads_and_defaults(void) {
    unsetenv("HU_TEST_GATE_MODE");
    HU_ASSERT_EQ((int)hu_gate_mode_from_env("HU_TEST_GATE_MODE", HU_GATE_SHADOW),
                 (int)HU_GATE_SHADOW);
    setenv("HU_TEST_GATE_MODE", "live", 1);
    HU_ASSERT_EQ((int)hu_gate_mode_from_env("HU_TEST_GATE_MODE", HU_GATE_OFF),
                 (int)HU_GATE_LIVE);
    unsetenv("HU_TEST_GATE_MODE");
}

void run_gate_mode_tests(void) {
    HU_TEST_SUITE("core gate mode");
    HU_RUN_TEST(gate_parse_unset_and_empty_take_callers_default);
    HU_RUN_TEST(gate_parse_off_and_shadow);
    HU_RUN_TEST(gate_parse_live_aliases);
    HU_RUN_TEST(gate_parse_is_case_insensitive);
    HU_RUN_TEST(gate_parse_unknown_fails_closed_regardless_of_default);
    HU_RUN_TEST(gate_from_env_reads_and_defaults);
}
