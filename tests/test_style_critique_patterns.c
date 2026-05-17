/* US-7.9 AC-7.9.5 — pure pattern-matcher tests for the style
 * self-critique.  No provider, no agent, no allocator: just the
 * literal needle matcher.
 *
 * Each test resets the test-only counters and asserts both the
 * violated_rule_out fan-out and the check-invocation accounting. */

#include "test_framework.h"

#include "human/persona/style_critique.h"

#include <string.h>

static void rules_one(const char *r, char **out) {
    out[0] = (char *)r;
}

static void test_prefix_sure_fires(void) {
    hu_style_critique_test_reset();
    char *rules[1];
    rules_one("never start with 'Sure!'", rules);
    const char *draft = "Sure! Here you go.";
    const char *vr = NULL;
    size_t vrl = 0;
    HU_ASSERT_EQ(hu_style_critique_check(draft, strlen(draft), rules, 1, &vr, &vrl), HU_OK);
    HU_ASSERT_NOT_NULL(vr);
    HU_ASSERT_EQ(vrl, strlen(rules[0]));
}

static void test_prefix_sure_word_boundary(void) {
    hu_style_critique_test_reset();
    char *rules[1];
    /* Use bare 'sure' to exercise the word-boundary rule directly —
     * "Surely yes." must NOT fire even though 'sure' is a prefix
     * substring. */
    rules_one("never start with 'sure'", rules);
    const char *draft = "Surely yes.";
    const char *vr = NULL;
    size_t vrl = 0;
    HU_ASSERT_EQ(hu_style_critique_check(draft, strlen(draft), rules, 1, &vr, &vrl), HU_OK);
    HU_ASSERT_NULL(vr);
    HU_ASSERT_EQ(vrl, (size_t)0);
}

static void test_prefix_sure_leading_whitespace(void) {
    hu_style_critique_test_reset();
    char *rules[1];
    rules_one("never start with 'Sure!'", rules);
    const char *draft = "  Sure! ...";
    const char *vr = NULL;
    size_t vrl = 0;
    HU_ASSERT_EQ(hu_style_critique_check(draft, strlen(draft), rules, 1, &vr, &vrl), HU_OK);
    HU_ASSERT_NOT_NULL(vr);
}

static void test_substring_em_dash(void) {
    hu_style_critique_test_reset();
    char *rules[1];
    rules_one("no em-dashes", rules);
    const char *draft = "yes \xE2\x80\x94 agreed";
    const char *vr = NULL;
    size_t vrl = 0;
    HU_ASSERT_EQ(hu_style_critique_check(draft, strlen(draft), rules, 1, &vr, &vrl), HU_OK);
    HU_ASSERT_NOT_NULL(vr);
}

static void test_substring_em_dash_clean(void) {
    hu_style_critique_test_reset();
    char *rules[1];
    rules_one("no em-dashes", rules);
    const char *draft = "yes - agreed";
    const char *vr = NULL;
    size_t vrl = 0;
    HU_ASSERT_EQ(hu_style_critique_check(draft, strlen(draft), rules, 1, &vr, &vrl), HU_OK);
    HU_ASSERT_NULL(vr);
}

static void test_case_insensitive_prefix(void) {
    hu_style_critique_test_reset();
    char *rules[1];
    rules_one("never start with 'sure!'", rules);
    const char *draft = "SURE! yo";
    const char *vr = NULL;
    size_t vrl = 0;
    HU_ASSERT_EQ(hu_style_critique_check(draft, strlen(draft), rules, 1, &vr, &vrl), HU_OK);
    HU_ASSERT_NOT_NULL(vr);
}

static void test_emoji_alias(void) {
    hu_style_critique_test_reset();
    char *rules[1];
    rules_one("no emoji", rules);
    /* Thumbs-up: U+1F44D → \xF0\x9F\x91\x8D — matches alias \xF0\x9F. */
    const char *draft = "ok \xF0\x9F\x91\x8D";
    const char *vr = NULL;
    size_t vrl = 0;
    HU_ASSERT_EQ(hu_style_critique_check(draft, strlen(draft), rules, 1, &vr, &vrl), HU_OK);
    HU_ASSERT_NOT_NULL(vr);
}

static void test_empty_rules_no_match(void) {
    hu_style_critique_test_reset();
    const char *draft = "anything goes — really!";
    const char *vr = NULL;
    size_t vrl = 0;
    HU_ASSERT_EQ(hu_style_critique_check(draft, strlen(draft), NULL, 0, &vr, &vrl), HU_OK);
    HU_ASSERT_NULL(vr);
    /* Even with zero rules, the function still counts as invoked. */
    HU_ASSERT_EQ(hu_style_critique_test_check_invocations, 1);
}

static void test_null_draft_returns_invalid_arg(void) {
    hu_style_critique_test_reset();
    const char *vr = NULL;
    size_t vrl = 0;
    HU_ASSERT_EQ(hu_style_critique_check(NULL, 0, NULL, 0, &vr, &vrl), HU_ERR_INVALID_ARGUMENT);
}

static void test_unknown_rule_phrase_falls_back_to_quoted_span(void) {
    hu_style_critique_test_reset();
    char *rules[1];
    rules_one("avoid the phrase 'as an AI'", rules);
    const char *draft = "As an AI, I would like to say hi.";
    const char *vr = NULL;
    size_t vrl = 0;
    HU_ASSERT_EQ(hu_style_critique_check(draft, strlen(draft), rules, 1, &vr, &vrl), HU_OK);
    HU_ASSERT_NOT_NULL(vr);
}

static void test_multiple_rules_first_match_wins(void) {
    hu_style_critique_test_reset();
    char *rules[2];
    rules[0] = (char *)"never start with 'Sure!'";
    rules[1] = (char *)"no em-dashes";
    const char *draft = "Hello — friend.";
    const char *vr = NULL;
    size_t vrl = 0;
    HU_ASSERT_EQ(hu_style_critique_check(draft, strlen(draft), rules, 2, &vr, &vrl), HU_OK);
    HU_ASSERT_NOT_NULL(vr);
    /* The em-dash rule (index 1) must be the one that fires. */
    HU_ASSERT_EQ(vrl, strlen(rules[1]));
}

static void test_word_boundary_punct_end_of_needle(void) {
    /* Needle ending in '!' (non-word char) → boundary auto-satisfied;
     * draft "Sure!Then" must still fire. */
    hu_style_critique_test_reset();
    char *rules[1];
    rules_one("never start with 'Sure!'", rules);
    const char *draft = "Sure!Then more";
    const char *vr = NULL;
    size_t vrl = 0;
    HU_ASSERT_EQ(hu_style_critique_check(draft, strlen(draft), rules, 1, &vr, &vrl), HU_OK);
    HU_ASSERT_NOT_NULL(vr);
}

void run_style_critique_patterns_tests(void);
void run_style_critique_patterns_tests(void) {
    HU_TEST_SUITE("StyleCritiquePatterns");
    HU_RUN_TEST(test_prefix_sure_fires);
    HU_RUN_TEST(test_prefix_sure_word_boundary);
    HU_RUN_TEST(test_prefix_sure_leading_whitespace);
    HU_RUN_TEST(test_substring_em_dash);
    HU_RUN_TEST(test_substring_em_dash_clean);
    HU_RUN_TEST(test_case_insensitive_prefix);
    HU_RUN_TEST(test_emoji_alias);
    HU_RUN_TEST(test_empty_rules_no_match);
    HU_RUN_TEST(test_null_draft_returns_invalid_arg);
    HU_RUN_TEST(test_unknown_rule_phrase_falls_back_to_quoted_span);
    HU_RUN_TEST(test_multiple_rules_first_match_wins);
    HU_RUN_TEST(test_word_boundary_punct_end_of_needle);
}
