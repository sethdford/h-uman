/* tests/test_eval_shape.c
 *
 * Unit tests for src/eval/shape.c — the C-side deterministic shape
 * classifier. Mirrors the Python tool scripts/eval_shape_classifier.py;
 * any disagreement between the two is a parity bug.
 *
 * 2026-05-18 (M1 + U2 from the audit follow-up scorecard).
 */

#include "human/eval/shape.h"
#include "test_framework.h"
#include <string.h>

static void test_shape_null_response_fails(void) {
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    HU_ASSERT_EQ(hu_shape_classify(NULL, 0, HU_SHAPE_CHANNEL_IMESSAGE, &r), HU_OK);
    HU_ASSERT_EQ((int)r.passed, 0);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_NULL_RESPONSE) != 0);
}

static void test_shape_empty_response_fails(void) {
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    const char *resp = "   \n\t  ";
    HU_ASSERT_EQ(hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r), HU_OK);
    HU_ASSERT_EQ((int)r.passed, 0);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_EMPTY_RESPONSE) != 0);
}

static void test_shape_peak_seth_passes(void) {
    /* Real Seth-voice responses captured from the persona-eval audit chain. */
    hu_shape_result_t r;
    const char *responses[] = {
        "yeah just sent it",
        "wild is one way to put it lol",
        "ha typical",
        "no worries at all.",
        "damn that's brutal. what did they even say?",
        "yeah i'll be around",
    };
    for (size_t i = 0; i < sizeof(responses) / sizeof(responses[0]); i++) {
        memset(&r, 0, sizeof(r));
        HU_ASSERT_EQ(
            hu_shape_classify(responses[i], strlen(responses[i]), HU_SHAPE_CHANNEL_IMESSAGE, &r),
            HU_OK);
        HU_ASSERT_EQ((int)r.passed, 1);
        HU_ASSERT_TRUE(r.score >= 0.99);
    }
}

static void test_shape_ai_assistant_depending_on_fails_imessage(void) {
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    const char *resp = "Depending on if you're actually free or not, here are a few options:\n"
                       "**If you are:**\n"
                       "* yeah\n"
                       "* sure";
    HU_ASSERT_EQ(hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r), HU_OK);
    HU_ASSERT_EQ((int)r.passed, 0);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_DEPENDING_ON) != 0);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_BULLET_LIST) != 0);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_BOLD_MARKDOWN) != 0);
}

static void test_shape_too_long_imessage(void) {
    /* iMessage threshold: too_long=250, way_too_long=500 */
    char buf[600];
    memset(buf, 'a', sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    HU_ASSERT_EQ(hu_shape_classify(buf, 599, HU_SHAPE_CHANNEL_IMESSAGE, &r), HU_OK);
    HU_ASSERT_EQ((int)r.passed, 0);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_WAY_TOO_LONG) != 0);
}

static void test_shape_slack_allows_markdown(void) {
    /* Slack rule: markdown allowed; same prompt that fails iMessage passes Slack. */
    const char *resp = "Here's the summary:\n"
                       "* point one\n"
                       "* point two";
    hu_shape_result_t r_im, r_sl;
    memset(&r_im, 0, sizeof(r_im));
    memset(&r_sl, 0, sizeof(r_sl));
    hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r_im);
    hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_SLACK, &r_sl);
    /* iMessage flags markdown; Slack doesn't. */
    HU_ASSERT_TRUE((r_im.fail_flags & HU_SHAPE_FAIL_BULLET_LIST) != 0);
    HU_ASSERT_TRUE((r_sl.fail_flags & HU_SHAPE_FAIL_BULLET_LIST) == 0);
    HU_ASSERT_TRUE(r_sl.score > r_im.score);
}

static void test_shape_email_allows_openers(void) {
    /* Email channel: AI-style openers allowed (sometimes legitimate). */
    const char *resp = "Certainly, here are the details you requested.";
    hu_shape_result_t r_im, r_email;
    memset(&r_im, 0, sizeof(r_im));
    memset(&r_email, 0, sizeof(r_email));
    hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r_im);
    hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_EMAIL, &r_email);
    HU_ASSERT_TRUE((r_im.fail_flags & HU_SHAPE_FAIL_CERTAINLY) != 0);
    HU_ASSERT_TRUE((r_email.fail_flags & HU_SHAPE_FAIL_CERTAINLY) == 0);
}

static void test_shape_excessive_emoji_fails(void) {
    /* M5: pure emoji string violates seth.json "ZERO emoji on most msgs". */
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    const char *resp = "\xF0\x9F\x98\x82\xF0\x9F\x94\xA5\xF0\x9F\x92\xAF"; /* 😂🔥💯 */
    HU_ASSERT_EQ(hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r), HU_OK);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_EXCESSIVE_EMOJI) != 0);
}

static void test_shape_channel_from_string_case_insensitive(void) {
    HU_ASSERT_EQ(hu_shape_channel_from_string("imessage", 8), HU_SHAPE_CHANNEL_IMESSAGE);
    HU_ASSERT_EQ(hu_shape_channel_from_string("IMessage", 8), HU_SHAPE_CHANNEL_IMESSAGE);
    HU_ASSERT_EQ(hu_shape_channel_from_string("slack", 5), HU_SHAPE_CHANNEL_SLACK);
    HU_ASSERT_EQ(hu_shape_channel_from_string("DISCORD", 7), HU_SHAPE_CHANNEL_DISCORD);
    HU_ASSERT_EQ(hu_shape_channel_from_string("email", 5), HU_SHAPE_CHANNEL_EMAIL);
    /* Unknown channel falls back to iMessage (strictest). */
    HU_ASSERT_EQ(hu_shape_channel_from_string("xyz", 3), HU_SHAPE_CHANNEL_IMESSAGE);
    HU_ASSERT_EQ(hu_shape_channel_from_string(NULL, 0), HU_SHAPE_CHANNEL_IMESSAGE);
}

static void test_shape_numbered_list_flagged_imessage(void) {
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    const char *resp = "Sure!\n1. first\n2. second\n3. third";
    hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_NUMBERED_LIST) != 0);
    HU_ASSERT_EQ((int)r.passed, 0);
}

static void test_shape_header_flagged_imessage(void) {
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    const char *resp = "Sure thing\n\n## Section\n\ntext here";
    hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_HEADER) != 0);
}

void run_eval_shape_tests(void) {
    HU_TEST_SUITE("eval shape classifier");
    HU_RUN_TEST(test_shape_null_response_fails);
    HU_RUN_TEST(test_shape_empty_response_fails);
    HU_RUN_TEST(test_shape_peak_seth_passes);
    HU_RUN_TEST(test_shape_ai_assistant_depending_on_fails_imessage);
    HU_RUN_TEST(test_shape_too_long_imessage);
    HU_RUN_TEST(test_shape_slack_allows_markdown);
    HU_RUN_TEST(test_shape_email_allows_openers);
    HU_RUN_TEST(test_shape_excessive_emoji_fails);
    HU_RUN_TEST(test_shape_channel_from_string_case_insensitive);
    HU_RUN_TEST(test_shape_numbered_list_flagged_imessage);
    HU_RUN_TEST(test_shape_header_flagged_imessage);
}
