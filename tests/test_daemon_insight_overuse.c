/* Insight overuse — the PAS-style counter-metric (arXiv 2609.04676).
 *
 * HU_INSIGHT_STREAM went live to move specificity; nothing measured whether
 * the twin brings memory up when the turn didn't ask for it. These tests pin
 * the pure count (whole-word, case-folded, deduped, digits and stop words
 * skipped), the gate (unset -> OFF; shadow never touches memory), and the
 * scan's refusal to invent a number when the insight stream is not live. */
#include "human/agent/memory_loader.h"
#include "human/core/allocator.h"
#include "human/daemon/insight_overuse.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

static const char k_block[] =
    "- her sister mindy lives in tampa (as of Sep 2026)\n"
    "- planning a cabo trip in november (as of Aug 2026)\n"
    "- hates cilantro, loves the taco truck on central (as of Jul 2026)\n";

static void count_splits_surfaced_into_prompted_and_unprompted(void) {
    hu_insight_overuse_t r;
    const char *inbound = "how's mindy doing? we should do tacos this week";
    const char *reply =
        "mindy's good, still in tampa. and yes the taco truck on central, cabo talk after";
    HU_ASSERT_EQ(hu_insight_overuse_count(k_block, sizeof(k_block) - 1, inbound, strlen(inbound),
                                          reply, strlen(reply), &r),
                 HU_OK);
    /* injected: sister mindy lives tampa planning cabo trip november hates
     * cilantro loves taco truck central = 14 (digits "2026" skipped, "her"/"in"
     * /"the"/"on" too short, "as"/"of" too short) */
    HU_ASSERT_EQ(r.injected, (size_t)14);
    /* surfaced in reply: mindy tampa taco truck central cabo = 6 */
    HU_ASSERT_EQ(r.surfaced, (size_t)6);
    /* prompted (also in inbound): mindy only — "tacos" is not the whole word "taco" */
    HU_ASSERT_EQ(r.prompted, (size_t)1);
}

static void count_is_whole_word_and_case_folded(void) {
    hu_insight_overuse_t r;
    const char *block = "- got a new mac for work (as of Sep 2026)";
    /* "stomach" must not count as "mac"; the token is >=4 anyway ("work") */
    HU_ASSERT_EQ(hu_insight_overuse_count(block, strlen(block), "", 0,
                                          "my stomach hurts. WORK was fine", 31, &r),
                 HU_OK);
    HU_ASSERT_EQ(r.injected, (size_t)1); /* "work" (mac is 3 chars, skipped) */
    HU_ASSERT_EQ(r.surfaced, (size_t)1); /* WORK matches work */
    HU_ASSERT_EQ(r.prompted, (size_t)0);
}

static void count_dedupes_repeated_tokens_and_skips_stop_words(void) {
    hu_insight_overuse_t r;
    const char *block = "- tampa tampa tampa, really just about tampa (as of Sep 2026)";
    HU_ASSERT_EQ(hu_insight_overuse_count(block, strlen(block), NULL, 0, "tampa!", 6, &r), HU_OK);
    HU_ASSERT_EQ(r.injected, (size_t)1); /* "really"/"just"/"about" are stop words */
    HU_ASSERT_EQ(r.surfaced, (size_t)1);
}

static void count_with_no_block_is_all_zero_and_null_out_is_refused(void) {
    hu_insight_overuse_t r = {9, 9, 9};
    HU_ASSERT_EQ(hu_insight_overuse_count(NULL, 0, "x", 1, "y", 1, &r), HU_OK);
    HU_ASSERT_EQ(r.injected + r.surfaced + r.prompted, (size_t)0);
    HU_ASSERT_EQ(hu_insight_overuse_count("- a", 3, "x", 1, "y", 1, NULL), HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(hu_insight_overuse_count("- tampa", 7, NULL, 3, "y", 1, &r),
                 HU_ERR_INVALID_ARGUMENT);
}

static void mode_defaults_off_and_parses_shadow(void) {
    const char *old = getenv("HU_INSIGHT_OVERUSE");
    char *saved = old ? strdup(old) : NULL;
    unsetenv("HU_INSIGHT_OVERUSE");
    HU_ASSERT_EQ((int)hu_insight_overuse_mode(), (int)HU_GATE_OFF);
    setenv("HU_INSIGHT_OVERUSE", "shadow", 1);
    HU_ASSERT_EQ((int)hu_insight_overuse_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_INSIGHT_OVERUSE", "bogus", 1);
    HU_ASSERT_EQ((int)hu_insight_overuse_mode(), (int)HU_GATE_OFF); /* fail closed */
    if (saved) {
        setenv("HU_INSIGHT_OVERUSE", saved, 1);
        free(saved);
    } else {
        unsetenv("HU_INSIGHT_OVERUSE");
    }
}

static void scan_off_does_no_work_and_bad_args_are_refused(void) {
    hu_allocator_t alloc = hu_system_allocator();
    hu_insight_overuse_t r = {1, 1, 1};
    /* OFF: no memory needed, no error, zeros */
    HU_ASSERT_EQ(hu_daemon_insight_overuse_scan(NULL, &alloc, "+1", 2, "a", 1, "b", 1, HU_GATE_OFF,
                                                NULL, &r),
                 HU_OK);
    HU_ASSERT_EQ(r.injected + r.surfaced + r.prompted, (size_t)0);
    /* SHADOW with no memory: refuse rather than fabricate zeros */
    HU_ASSERT_EQ(hu_daemon_insight_overuse_scan(NULL, &alloc, "+1", 2, "a", 1, "b", 1,
                                                HU_GATE_SHADOW, NULL, &r),
                 HU_ERR_INVALID_ARGUMENT);
}

static void scan_reports_zeros_when_insight_stream_is_not_live(void) {
    /* Nothing was injected unless HU_INSIGHT_STREAM is live; the scan must
     * not read memory then (a fake non-NULL pointer would crash if it did). */
    hu_allocator_t alloc = hu_system_allocator();
    hu_insight_overuse_t r = {1, 1, 1};
    hu_memory_loader_set_insight_mode_for_test((int)HU_GATE_OFF);
    int dummy = 0;
    HU_ASSERT_EQ(hu_daemon_insight_overuse_scan((hu_memory_t *)&dummy, &alloc, "+15550001", 9, "a",
                                                1, "b", 1, HU_GATE_SHADOW, NULL, &r),
                 HU_OK);
    HU_ASSERT_EQ(r.injected + r.surfaced + r.prompted, (size_t)0);
    hu_memory_loader_set_insight_mode_for_test(-1);
}

void run_daemon_insight_overuse_tests(void) {
    HU_TEST_SUITE("daemon_insight_overuse");
    HU_RUN_TEST(count_splits_surfaced_into_prompted_and_unprompted);
    HU_RUN_TEST(count_is_whole_word_and_case_folded);
    HU_RUN_TEST(count_dedupes_repeated_tokens_and_skips_stop_words);
    HU_RUN_TEST(count_with_no_block_is_all_zero_and_null_out_is_refused);
    HU_RUN_TEST(mode_defaults_off_and_parses_shadow);
    HU_RUN_TEST(scan_off_does_no_work_and_bad_args_are_refused);
    HU_RUN_TEST(scan_reports_zeros_when_insight_stream_is_not_live);
}
