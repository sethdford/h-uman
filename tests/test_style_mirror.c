#include "human/core/error.h"
#include "human/persona/style_mirror.h"
#include "test_framework.h"
#include <string.h>

/* Sprint 6 US-19: unit tests for hu_style_mirror_apply */

/* ── null / guard tests ───────────────────────────────────────────── */

static void mirror_null_buf_returns_invalid(void) {
    size_t len = 5;
    const char *msgs[] = {"hey", "yeah"};
    hu_error_t err = hu_style_mirror_apply(NULL, &len, msgs, 2, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void mirror_null_len_returns_invalid(void) {
    char buf[64];
    strncpy(buf, "Hello.", sizeof(buf) - 1);
    const char *msgs[] = {"hey", "yeah"};
    hu_error_t err = hu_style_mirror_apply(buf, NULL, msgs, 2, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void mirror_fewer_than_two_messages_is_noop(void) {
    char buf[64];
    strncpy(buf, "Hey there.", sizeof(buf) - 1);
    size_t len = strlen(buf);
    const char *msgs[] = {"hey"};

    hu_style_mirror_apply(buf, &len, msgs, 1, NULL);

    /* No signal — buffer unchanged */
    HU_ASSERT_STR_EQ(buf, "Hey there.");
}

/* ── lowercasing tests ────────────────────────────────────────────── */

static void mirror_lowercases_when_partner_lowercases(void) {
    /* Partner: 3 of 3 messages start lowercase → ratio 1.0 >= 0.7 */
    const char *partner[] = {"hey", "what's up", "cool"};
    char buf[64];
    strncpy(buf, "Hi there.", sizeof(buf) - 1);
    size_t len = strlen(buf);

    /* "Hi" is 2 chars — within the 1-3 threshold → should be lowercased */
    hu_style_mirror_apply(buf, &len, partner, 3, NULL);
    HU_ASSERT_TRUE(buf[0] == 'h'); /* 'H' → 'h' */
}

static void mirror_no_change_when_partner_capitalizes(void) {
    /* Partner: all start uppercase → ratio 0.0 < 0.7 → no lowercase rule */
    const char *partner[] = {"Hello.", "Good morning.", "Cool."};
    char buf[64];
    strncpy(buf, "Hey there.", sizeof(buf) - 1);
    size_t len = strlen(buf);
    const char original[] = "Hey there.";

    hu_style_mirror_apply(buf, &len, partner, 3, NULL);
    HU_ASSERT_STR_EQ(buf, original);
}

static void mirror_preserves_long_words_at_sentence_start(void) {
    /* Partner all lowercase → rule triggers, but "Jordan" is 6 chars → not touched */
    const char *partner[] = {"hey", "yeah", "cool"};
    char buf[64];
    strncpy(buf, "Jordan said hi.", sizeof(buf) - 1);
    size_t len = strlen(buf);

    hu_style_mirror_apply(buf, &len, partner, 3, NULL);

    /* "Jordan" (6 chars) must survive untouched */
    HU_ASSERT_TRUE(buf[0] == 'J');
}

static void mirror_lowercases_short_words_only(void) {
    /* Partner all lowercase → "Hi" (2 chars) should be lowercased,
     * "Jordan" (6 chars) in second sentence should NOT be lowercased */
    const char *partner[] = {"hey", "yeah", "cool"};
    char buf[64];
    strncpy(buf, "Hi there. Jordan said hi.", sizeof(buf) - 1);
    size_t len = strlen(buf);

    hu_style_mirror_apply(buf, &len, partner, 3, NULL);

    /* "Hi" → "hi" */
    HU_ASSERT_TRUE(buf[0] == 'h');
    /* Find "Jordan" — should still be 'J' */
    const char *jordan = strstr(buf, "Jordan");
    HU_ASSERT_NOT_NULL(jordan);
    HU_ASSERT_TRUE(jordan[0] == 'J');
}

/* ── period stripping tests ───────────────────────────────────────── */

static void mirror_strips_periods_when_partner_skips(void) {
    /* Partner: none end with '.' → no_period_ratio = 1.0 >= 0.7 */
    const char *partner[] = {"hey", "yeah", "cool"};
    char buf[64];
    strncpy(buf, "yeah totally.", sizeof(buf) - 1);
    size_t len = strlen(buf);
    hu_style_mirror_report_t report;

    hu_style_mirror_apply(buf, &len, partner, 3, &report);

    HU_ASSERT_TRUE(report.periods_stripped);
    /* Trailing period should be gone */
    HU_ASSERT_TRUE(len > 0 && buf[len - 1] != '.');
    HU_ASSERT_STR_EQ(buf, "yeah totally");
}

static void mirror_preserves_periods_when_partner_uses_them(void) {
    /* Partner: all end with '.' → no_period_ratio = 0.0 < 0.7 → no strip */
    const char *partner[] = {"Hello.", "Good morning.", "Cool."};
    char buf[64];
    strncpy(buf, "Yeah totally.", sizeof(buf) - 1);
    size_t len = strlen(buf);

    hu_style_mirror_apply(buf, &len, partner, 3, NULL);

    /* Period must be preserved */
    HU_ASSERT_TRUE(len > 0 && buf[len - 1] == '.');
}

static void mirror_does_not_strip_question_mark(void) {
    /* Partner skips periods → rule fires, but should NOT strip '?' */
    const char *partner[] = {"hey", "yeah", "cool"};
    char buf[64];
    strncpy(buf, "Is that right?", sizeof(buf) - 1);
    size_t len = strlen(buf);

    hu_style_mirror_apply(buf, &len, partner, 3, NULL);

    HU_ASSERT_TRUE(len > 0 && buf[len - 1] == '?');
}

static void mirror_does_not_strip_exclamation_mark(void) {
    const char *partner[] = {"hey", "yeah", "cool"};
    char buf[64];
    strncpy(buf, "That's great!", sizeof(buf) - 1);
    size_t len = strlen(buf);

    hu_style_mirror_apply(buf, &len, partner, 3, NULL);

    HU_ASSERT_TRUE(len > 0 && buf[len - 1] == '!');
}

/* ── report struct test ───────────────────────────────────────────── */

static void mirror_report_populated_correctly(void) {
    const char *partner[] = {"hey", "yeah", "cool"};
    char buf[64];
    strncpy(buf, "Hi there.", sizeof(buf) - 1);
    size_t len = strlen(buf);
    hu_style_mirror_report_t report;

    hu_style_mirror_apply(buf, &len, partner, 3, &report);

    HU_ASSERT_TRUE(report.lowercased_applied);
    HU_ASSERT_TRUE(report.periods_stripped);
    HU_ASSERT_TRUE(report.edits >= 2);
}

void run_style_mirror_tests(void) {
    HU_TEST_SUITE("StyleMirror");
    HU_RUN_TEST(mirror_null_buf_returns_invalid);
    HU_RUN_TEST(mirror_null_len_returns_invalid);
    HU_RUN_TEST(mirror_fewer_than_two_messages_is_noop);
    HU_RUN_TEST(mirror_lowercases_when_partner_lowercases);
    HU_RUN_TEST(mirror_no_change_when_partner_capitalizes);
    HU_RUN_TEST(mirror_preserves_long_words_at_sentence_start);
    HU_RUN_TEST(mirror_lowercases_short_words_only);
    HU_RUN_TEST(mirror_strips_periods_when_partner_skips);
    HU_RUN_TEST(mirror_preserves_periods_when_partner_uses_them);
    HU_RUN_TEST(mirror_does_not_strip_question_mark);
    HU_RUN_TEST(mirror_does_not_strip_exclamation_mark);
    HU_RUN_TEST(mirror_report_populated_correctly);
}
