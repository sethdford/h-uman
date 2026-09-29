/* Wave 3 — continuous persona learning tests.
 * Covers the pure 24h cadence predicate and the refresh function's safety
 * contract (invalid args + never-wipe-on-failure: a bogus history path returns
 * non-OK and writes nothing). Real extraction needs SQLITE+ML + a live history
 * DB; those paths are exercised by the lora-persona CLI + e2e suites. */

#include "human/core/allocator.h"
#include "human/persona.h"
#include "test_framework.h"

#include <string.h>

static void refresh_should_run_disabled_is_false(void) {
    HU_ASSERT_FALSE(hu_persona_refresh_should_run(false, 1000000, 0));
    HU_ASSERT_FALSE(hu_persona_refresh_should_run(false, 1000000, 1));
}

static void refresh_should_run_never_run_is_true(void) {
    /* enabled + last_run<=0 → run now (first time) */
    HU_ASSERT_TRUE(hu_persona_refresh_should_run(true, 1000000, 0));
    HU_ASSERT_TRUE(hu_persona_refresh_should_run(true, 1000000, -5));
}

static void refresh_should_run_recent_is_false(void) {
    int64_t now = 1000000;
    /* 12h ago < 24h cadence → don't run yet */
    HU_ASSERT_FALSE(hu_persona_refresh_should_run(true, now, now - (int64_t)12 * 3600));
}

static void refresh_should_run_stale_is_true(void) {
    int64_t now = 1000000;
    /* exactly 24h and beyond → run */
    HU_ASSERT_TRUE(hu_persona_refresh_should_run(true, now, now - (int64_t)24 * 3600));
    HU_ASSERT_TRUE(hu_persona_refresh_should_run(true, now, now - (int64_t)48 * 3600));
}

static void refresh_rejects_invalid_args(void) {
    hu_allocator_t a = hu_system_allocator();
    size_t total = 99;
    HU_ASSERT_EQ(hu_persona_refresh_example_banks(NULL, "seth", 4, "/tmp/x.db", 0, &total),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_refresh_example_banks(&a, NULL, 0, "/tmp/x.db", 0, &total),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_refresh_example_banks(&a, "seth", 4, NULL, 0, &total),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_persona_refresh_example_banks(&a, "seth", 4, "", 0, &total),
                 HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(total, (size_t)0);
}

static void refresh_bogus_history_writes_nothing(void) {
    /* A nonexistent history DB must NOT produce a write and must report a count
     * of 0 (extract fails with NOT_SUPPORTED on non-ML builds, or IO on ML
     * builds — either way non-OK and the authored persona is left untouched). */
    hu_allocator_t a = hu_system_allocator();
    size_t total = 123;
    hu_error_t err = hu_persona_refresh_example_banks(&a, "definitely-not-a-real-persona-xyz", 33,
                                                      "/nonexistent/history-xyz.db", 0, &total);
    HU_ASSERT(err != HU_OK);
    HU_ASSERT_EQ(total, (size_t)0);
}

/* Per-turn style reanalyze gate (2026-09-06 incident: the ungated call rewrote
 * the live persona through the struct-only writer and dropped contacts,
 * proactive, life_events, style_rules). Disabled must win at EVERY cadence
 * point, not just off-cadence counts. */
static void reanalyze_due_disabled_never_fires(void) {
    size_t cadence_points[] = {10, 20, 25, 50, 75, 100, 150, 200, 1000};
    for (size_t i = 0; i < sizeof(cadence_points) / sizeof(cadence_points[0]); i++)
        HU_ASSERT_FALSE(hu_persona_style_reanalyze_due(false, cadence_points[i]));
}

static void reanalyze_due_zero_history_never_fires(void) {
    HU_ASSERT_FALSE(hu_persona_style_reanalyze_due(true, 0));
}

static void reanalyze_due_enabled_keeps_adaptive_cadence(void) {
    /* early: every 10 up to 20 */
    HU_ASSERT_TRUE(hu_persona_style_reanalyze_due(true, 10));
    HU_ASSERT_TRUE(hu_persona_style_reanalyze_due(true, 20));
    HU_ASSERT_FALSE(hu_persona_style_reanalyze_due(true, 5));
    HU_ASSERT_FALSE(hu_persona_style_reanalyze_due(true, 15));
    /* mid: every 25 up to 100 (30 would have matched %10 — must not) */
    HU_ASSERT_TRUE(hu_persona_style_reanalyze_due(true, 25));
    HU_ASSERT_TRUE(hu_persona_style_reanalyze_due(true, 100));
    HU_ASSERT_FALSE(hu_persona_style_reanalyze_due(true, 30));
    HU_ASSERT_FALSE(hu_persona_style_reanalyze_due(true, 90));
    /* steady: every 50 after 100 (125 would have matched %25 — must not) */
    HU_ASSERT_TRUE(hu_persona_style_reanalyze_due(true, 150));
    HU_ASSERT_TRUE(hu_persona_style_reanalyze_due(true, 1000));
    HU_ASSERT_FALSE(hu_persona_style_reanalyze_due(true, 125));
    HU_ASSERT_FALSE(hu_persona_style_reanalyze_due(true, 175));
}

void run_persona_refresh_tests(void) {
    HU_TEST_SUITE("persona refresh (Wave 3)");
    HU_RUN_TEST(refresh_should_run_disabled_is_false);
    HU_RUN_TEST(refresh_should_run_never_run_is_true);
    HU_RUN_TEST(refresh_should_run_recent_is_false);
    HU_RUN_TEST(refresh_should_run_stale_is_true);
    HU_RUN_TEST(refresh_rejects_invalid_args);
    HU_RUN_TEST(refresh_bogus_history_writes_nothing);
    HU_RUN_TEST(reanalyze_due_disabled_never_fires);
    HU_RUN_TEST(reanalyze_due_zero_history_never_fires);
    HU_RUN_TEST(reanalyze_due_enabled_keeps_adaptive_cadence);
}
