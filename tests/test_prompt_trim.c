/* tests/test_prompt_trim.c
 *
 * Truth table for the value-aware prompt-trim helpers
 * (hu_prompt_trim_mode_parse / hu_prompt_trim_plan / hu_prompt_trim_apply)
 * extracted per security-predicate-extraction.md so the trim decision is
 * pinned without assembling a real 16 KB prompt.
 *
 * Fixture buffer layout (46 bytes):
 *   [ 0.. 4] "PPPP\n"              protected head (persona)
 *   [ 5..20] "mmmmmmm\nMMMMMMM\n"  memory span (2 lines; head = oldest)
 *   [21..25] "xxxx\n"              protected filler between spans
 *   [26..35] "eeee\neeee\n"        self-exemplars span (priority 0)
 *   [36..40] "gggg\n"              graph span (priority 1)
 *   [41..45] "TAIL\n"              protected tail (CRITICAL REMINDER)
 *
 * Priority order (index in the spans array) is exemplars, graph, memory —
 * the pre-made design decision from the 2026-07-11 shrink plan.
 */

#include "human/agent/prompt_trim.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

#define FIX_TEXT "PPPP\nmmmmmmm\nMMMMMMM\nxxxx\neeee\neeee\ngggg\nTAIL\n"
#define FIX_LEN (sizeof(FIX_TEXT) - 1)

static const hu_prompt_trim_span_t k_fix_spans[3] = {
    {26, 10}, /* self-exemplars — trimmed first */
    {36, 5},  /* graph grounding — second */
    {5, 16},  /* memory context — last, head-first (oldest facts) */
};

static void test_trim_mode_parse_unknown_and_empty_default_off(void) {
    HU_ASSERT_EQ((int)hu_prompt_trim_mode_parse(NULL), (int)HU_PROMPT_TRIM_OFF);
    HU_ASSERT_EQ((int)hu_prompt_trim_mode_parse(""), (int)HU_PROMPT_TRIM_OFF);
    HU_ASSERT_EQ((int)hu_prompt_trim_mode_parse("off"), (int)HU_PROMPT_TRIM_OFF);
    HU_ASSERT_EQ((int)hu_prompt_trim_mode_parse("bogus"), (int)HU_PROMPT_TRIM_OFF);
    HU_ASSERT_EQ((int)hu_prompt_trim_mode_parse("LIVEish"), (int)HU_PROMPT_TRIM_OFF);
}

static void test_trim_mode_parse_shadow(void) {
    HU_ASSERT_EQ((int)hu_prompt_trim_mode_parse("shadow"), (int)HU_PROMPT_TRIM_SHADOW);
}

static void test_trim_mode_parse_live_aliases(void) {
    HU_ASSERT_EQ((int)hu_prompt_trim_mode_parse("live"), (int)HU_PROMPT_TRIM_LIVE);
    HU_ASSERT_EQ((int)hu_prompt_trim_mode_parse("on"), (int)HU_PROMPT_TRIM_LIVE);
    HU_ASSERT_EQ((int)hu_prompt_trim_mode_parse("1"), (int)HU_PROMPT_TRIM_LIVE);
}

static void test_trim_mode_env_reads_hu_prompt_trim(void) {
    unsetenv("HU_PROMPT_TRIM");
    HU_ASSERT_EQ((int)hu_prompt_trim_mode(), (int)HU_PROMPT_TRIM_OFF);
    setenv("HU_PROMPT_TRIM", "shadow", 1);
    HU_ASSERT_EQ((int)hu_prompt_trim_mode(), (int)HU_PROMPT_TRIM_SHADOW);
    setenv("HU_PROMPT_TRIM", "live", 1);
    HU_ASSERT_EQ((int)hu_prompt_trim_mode(), (int)HU_PROMPT_TRIM_LIVE);
    unsetenv("HU_PROMPT_TRIM");
}

static void test_trim_plan_under_budget_is_noop(void) {
    size_t cuts[3] = {77, 77, 77};
    size_t total = hu_prompt_trim_plan(FIX_TEXT, FIX_LEN, FIX_LEN, k_fix_spans, 3, cuts);
    HU_ASSERT_EQ(total, (size_t)0);
    HU_ASSERT_EQ(cuts[0], (size_t)0);
    HU_ASSERT_EQ(cuts[1], (size_t)0);
    HU_ASSERT_EQ(cuts[2], (size_t)0);
}

static void test_trim_plan_cuts_priority_zero_first_newline_rounded(void) {
    /* budget 40 -> overage 6. Exemplars (priority 0) alone must absorb it;
     * 6 bytes lands mid-line so the cut extends to the newline at byte 10
     * (the whole span). Graph and memory stay untouched. */
    size_t cuts[3] = {0, 0, 0};
    size_t total = hu_prompt_trim_plan(FIX_TEXT, FIX_LEN, 40, k_fix_spans, 3, cuts);
    HU_ASSERT_EQ(cuts[0], (size_t)10);
    HU_ASSERT_EQ(cuts[1], (size_t)0);
    HU_ASSERT_EQ(cuts[2], (size_t)0);
    HU_ASSERT_EQ(total, (size_t)10);
}

static void test_trim_plan_cascades_into_memory_head_first(void) {
    /* budget 30 -> overage 16. Exemplars (10) + graph (5) fully cut leaves
     * 1 byte for memory; head-first cut extends to the first newline, so
     * only the OLDEST memory line ("mmmmmmm\n", 8 bytes) goes. */
    size_t cuts[3] = {0, 0, 0};
    size_t total = hu_prompt_trim_plan(FIX_TEXT, FIX_LEN, 30, k_fix_spans, 3, cuts);
    HU_ASSERT_EQ(cuts[0], (size_t)10);
    HU_ASSERT_EQ(cuts[1], (size_t)5);
    HU_ASSERT_EQ(cuts[2], (size_t)8);
    HU_ASSERT_EQ(total, (size_t)23);
}

static void test_trim_plan_caps_at_available_span_bytes(void) {
    /* budget 1 -> overage 45, but only 31 trimmable bytes exist. The plan
     * exhausts every span and stops — protected head/tail are never part
     * of a plan. */
    size_t cuts[3] = {0, 0, 0};
    size_t total = hu_prompt_trim_plan(FIX_TEXT, FIX_LEN, 1, k_fix_spans, 3, cuts);
    HU_ASSERT_EQ(cuts[0], (size_t)10);
    HU_ASSERT_EQ(cuts[1], (size_t)5);
    HU_ASSERT_EQ(cuts[2], (size_t)16);
    HU_ASSERT_EQ(total, (size_t)31);
}

static void test_trim_plan_skips_absent_sections(void) {
    hu_prompt_trim_span_t spans[3] = {
        {0, 0},  /* exemplars absent */
        {36, 5}, /* graph present */
        {0, 0},  /* memory absent */
    };
    size_t cuts[3] = {0, 0, 0};
    size_t total = hu_prompt_trim_plan(FIX_TEXT, FIX_LEN, 43, spans, 3, cuts);
    HU_ASSERT_EQ(cuts[0], (size_t)0);
    HU_ASSERT_EQ(cuts[1], (size_t)5);
    HU_ASSERT_EQ(cuts[2], (size_t)0);
    HU_ASSERT_EQ(total, (size_t)5);
}

static void test_trim_plan_null_inputs_return_zero(void) {
    size_t cuts[3] = {0, 0, 0};
    HU_ASSERT_EQ(hu_prompt_trim_plan(NULL, FIX_LEN, 10, k_fix_spans, 3, cuts), (size_t)0);
    HU_ASSERT_EQ(hu_prompt_trim_plan(FIX_TEXT, FIX_LEN, 10, NULL, 3, cuts), (size_t)0);
    HU_ASSERT_EQ(hu_prompt_trim_plan(FIX_TEXT, FIX_LEN, 10, k_fix_spans, 3, NULL), (size_t)0);
}

static void test_trim_apply_removes_planned_middle_keeps_head_tail(void) {
    char buf[FIX_LEN + 1];
    memcpy(buf, FIX_TEXT, FIX_LEN + 1);
    size_t cuts[3] = {0, 0, 0};
    size_t planned = hu_prompt_trim_plan(buf, FIX_LEN, 30, k_fix_spans, 3, cuts);
    size_t new_len = hu_prompt_trim_apply(buf, FIX_LEN, k_fix_spans, 3, cuts);
    HU_ASSERT_EQ(new_len, FIX_LEN - planned);
    HU_ASSERT_EQ(strlen(buf), new_len); /* NUL-terminated at the new length */
    /* Protected content survives... */
    HU_ASSERT_TRUE(strstr(buf, "PPPP") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "xxxx") != NULL);
    HU_ASSERT_TRUE(strstr(buf, "TAIL") != NULL);
    /* ...the newest memory line survives (head-first = oldest dropped)... */
    HU_ASSERT_TRUE(strstr(buf, "MMMMMMM") != NULL);
    /* ...and the planned middle bytes are gone. */
    HU_ASSERT_TRUE(strstr(buf, "mmmmmmm") == NULL);
    HU_ASSERT_TRUE(strstr(buf, "eeee") == NULL);
    HU_ASSERT_TRUE(strstr(buf, "gggg") == NULL);
}

static void test_trim_apply_zero_cuts_is_identity(void) {
    char buf[FIX_LEN + 1];
    memcpy(buf, FIX_TEXT, FIX_LEN + 1);
    size_t cuts[3] = {0, 0, 0};
    size_t new_len = hu_prompt_trim_apply(buf, FIX_LEN, k_fix_spans, 3, cuts);
    HU_ASSERT_EQ(new_len, FIX_LEN);
    HU_ASSERT_STR_EQ(buf, FIX_TEXT);
}

static void test_positional_cap_under_budget_is_identity(void) {
    HU_ASSERT_EQ(hu_prompt_positional_cap_point(FIX_TEXT, FIX_LEN, FIX_LEN), FIX_LEN);
    HU_ASSERT_EQ(hu_prompt_positional_cap_point(FIX_TEXT, FIX_LEN, FIX_LEN + 100), FIX_LEN);
}

static void test_positional_cap_cuts_at_last_newline_within_budget(void) {
    /* budget 43 lands mid-"TAIL"; the last newline before it is at byte 41
     * (end of "gggg\n"), so the cut retreats there. */
    HU_ASSERT_EQ(hu_prompt_positional_cap_point(FIX_TEXT, FIX_LEN, 43), (size_t)41);
}

static void test_positional_cap_falls_back_to_hard_budget_without_newline(void) {
    /* no newline in the upper half of the budget window -> hard cap, mirroring
     * the cut < budget/2 fallback both turn paths used inline. */
    static const char no_nl[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    HU_ASSERT_EQ(hu_prompt_positional_cap_point(no_nl, sizeof(no_nl) - 1, 20), (size_t)20);
}

void run_prompt_trim_tests(void) {
    HU_TEST_SUITE("Prompt trim");
    HU_RUN_TEST(test_trim_mode_parse_unknown_and_empty_default_off);
    HU_RUN_TEST(test_trim_mode_parse_shadow);
    HU_RUN_TEST(test_trim_mode_parse_live_aliases);
    HU_RUN_TEST(test_trim_mode_env_reads_hu_prompt_trim);
    HU_RUN_TEST(test_trim_plan_under_budget_is_noop);
    HU_RUN_TEST(test_trim_plan_cuts_priority_zero_first_newline_rounded);
    HU_RUN_TEST(test_trim_plan_cascades_into_memory_head_first);
    HU_RUN_TEST(test_trim_plan_caps_at_available_span_bytes);
    HU_RUN_TEST(test_trim_plan_skips_absent_sections);
    HU_RUN_TEST(test_trim_plan_null_inputs_return_zero);
    HU_RUN_TEST(test_trim_apply_removes_planned_middle_keeps_head_tail);
    HU_RUN_TEST(test_trim_apply_zero_cuts_is_identity);
    HU_RUN_TEST(test_positional_cap_under_budget_is_identity);
    HU_RUN_TEST(test_positional_cap_cuts_at_last_newline_within_budget);
    HU_RUN_TEST(test_positional_cap_falls_back_to_hard_budget_without_newline);
}
