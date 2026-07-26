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

/* ── HU_SHAPE_FAIL_TRAILING_QUESTION — measurement-only ─────────────────
 * Measured 2026-07-26 over 689 real matched reply pairs: Seth ends a reply
 * with '?' 7.7% of the time; the model ran 42-70%. Four prompt layers already
 * forbid it and one added anti-pattern made it WORSE (55% -> 70%), so the
 * reflex is prompt-resistant and this flag is the SHADOW instrument. It must
 * never gate a send: no score penalty, absent from the persona mask. */

static void test_shape_trailing_question_is_flagged(void) {
    hu_shape_result_t r;
    const char *resp = "damn that sucks. how long till you take off?";
    HU_ASSERT_EQ(hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r), HU_OK);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_TRAILING_QUESTION) != 0);
}

static void test_shape_trailing_question_does_not_fail_the_response(void) {
    /* THE contract: flagged but still passing, and full score. If this ever
     * fails, someone gave the flag a penalty and it can now regenerate real
     * sends — that is the LIVE step and needs its own measurement first. */
    hu_shape_result_t r;
    const char *resp = "damn that sucks. how long till you take off?";
    HU_ASSERT_EQ(hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r), HU_OK);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_TRAILING_QUESTION) != 0);
    HU_ASSERT_TRUE(r.passed);
    HU_ASSERT_TRUE(r.score > 0.99);
}

static void test_shape_no_trailing_question_not_flagged(void) {
    hu_shape_result_t r;
    const char *resp = "damn that sucks";
    HU_ASSERT_EQ(hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r), HU_OK);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_TRAILING_QUESTION) == 0);
}

static void test_shape_bare_question_mark_not_flagged(void) {
    /* A lone "?" is a reaction, not the close-every-turn reflex. */
    hu_shape_result_t r;
    const char *resp = "?";
    HU_ASSERT_EQ(hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r), HU_OK);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_TRAILING_QUESTION) == 0);
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

/* ── Task 11 (AC-10): Per-contact learned length caps ────────────────── */

static void test_shape_close_contact_with_learned_cap_exceeds_universal(void) {
    /* AC-10: a close contact with learned_length_cap=300 can send a 300-char
     * iMessage, which would normally fail (universal imessage cap is 250). */
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    /* 280-char message — below universal cap but we'll test with a higher
     * learned_length_cap to show the close-contact override works. */
    const char *resp = "This is a longer message for a close contact. I've known this "
                       "person for a long time and we often have detailed conversations. "
                       "The universal cap for iMessage is 250 chars, but for close contacts "
                       "who Seth talks to regularly with longer messages, we allow up to the "
                       "learned baseline. This is about 280 characters.";

    HU_ASSERT_EQ(
        hu_shape_classify_ex(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, "close", 5, 350, &r),
        HU_OK);
    HU_ASSERT_EQ((int)r.passed, 1);
    HU_ASSERT_FALSE((r.fail_flags & HU_SHAPE_FAIL_TOO_LONG) != 0);
}

static void test_shape_close_contact_with_learned_cap_bullet_list_still_fails(void) {
    /* AC-10: structural fails (markdown) are NEVER allowed, even for close
     * contacts. A 200-char bullet list should fail even with a high learned cap. */
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    const char *resp = "Sure, here are the details:\n- first point about something\n"
                       "- second point about another thing\n- third point with more details";

    HU_ASSERT_EQ(
        hu_shape_classify_ex(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, "close", 5, 500, &r),
        HU_OK);
    HU_ASSERT_EQ((int)r.passed, 0);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_BULLET_LIST) != 0);
}

static void test_shape_without_learned_cap_reverts_to_universal(void) {
    /* AC-10: when learned_length_cap=0, even for close contacts, revert to
     * the universal channel cap. This ensures backward compatibility. */
    hu_shape_result_t r1;
    hu_shape_result_t r2;
    const char *resp =
        "This is a very long iMessage reply that runs far past the universal 250-character "
        "ceiling and even past the 500-character way-too-long threshold for the channel. "
        "Without any learned per-contact cap in play, the classifier must fall back to the "
        "universal iMessage bounds, which means a wall of text like this one is treated as a "
        "hard length violation rather than a mild one. We deliberately keep typing well beyond "
        "what any person would send in a single text so the byte count is unambiguously above "
        "five hundred characters, guaranteeing the way-too-long flag fires and the shape gate "
        "fails the response outright.";

    memset(&r1, 0, sizeof(r1));
    /* Classic call — way over the universal way_too_long (500) bound, so it must
     * fail: WAY_TOO_LONG forces passed=0 (a mere TOO_LONG only docks the score). */
    HU_ASSERT_EQ(hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r1), HU_OK);
    HU_ASSERT_EQ((int)r1.passed, 0);
    HU_ASSERT_TRUE((r1.fail_flags & HU_SHAPE_FAIL_WAY_TOO_LONG) != 0);

    memset(&r2, 0, sizeof(r2));
    /* _ex call with learned_cap=0 — should have same result as classic call. */
    HU_ASSERT_EQ(
        hu_shape_classify_ex(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, "close", 5, 0, &r2),
        HU_OK);
    HU_ASSERT_EQ((int)r2.passed, (int)r1.passed);
    HU_ASSERT_EQ(r2.fail_flags, r1.fail_flags);
}

static void test_shape_non_close_relationship_ignores_learned_cap(void) {
    /* AC-10: learned_length_cap only applies to "close" relationships. If the
     * relationship_stage is "friend", "family", or other, the universal cap
     * applies even with a high learned_cap. */
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    const char *resp =
        "This is a deliberately long message that sails past the iMessage universal cap of 250 "
        "characters and continues well beyond the 500-character way-too-long threshold too. Even "
        "though the caller supplies a learned cap of 500, the relationship stage here is 'friend' "
        "rather than 'close', so the per-contact override must not apply and the universal bounds "
        "govern. We keep padding the text so the total byte count is comfortably over five "
        "hundred, which guarantees the universal way-too-long flag fires for this non-close "
        "relationship and the shape check rejects it outright.";

    HU_ASSERT_EQ(
        hu_shape_classify_ex(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, "friend", 6, 500, &r),
        HU_OK);
    /* friend (not close) ignores the learned cap → universal way_too_long=500
     * applies; a >500-char reply is WAY_TOO_LONG, which forces passed=0. */
    HU_ASSERT_EQ((int)r.passed, 0);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_WAY_TOO_LONG) != 0);
}

/* 2026-05-29: AI self-disclosure / persona-break (HU_SHAPE_FAIL_AI_SELF_DISCLOSURE). */

static void test_shape_ai_self_disclosure_fails_imessage(void) {
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    const char *resp = "yeah, as an AI I can't do that";
    HU_ASSERT_EQ(hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r), HU_OK);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_AI_SELF_DISCLOSURE) != 0);
    HU_ASSERT_EQ((int)r.passed, 0); /* fatal persona-break */
}

static void test_shape_ai_no_access_disclaimer_fails_imessage(void) {
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    const char *resp = "I don't have access to your internal files.";
    HU_ASSERT_EQ(hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r), HU_OK);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_AI_SELF_DISCLOSURE) != 0);
    HU_ASSERT_EQ((int)r.passed, 0);
}

static void test_shape_ai_self_disclosure_allowed_on_email(void) {
    /* email allows AI-assistant register, so self-disclosure is NOT flagged. */
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    const char *resp = "As an AI, I would be happy to assist with your request.";
    HU_ASSERT_EQ(hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_EMAIL, &r), HU_OK);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_AI_SELF_DISCLOSURE) == 0);
    HU_ASSERT_EQ((int)r.passed, 1);
}

static void test_shape_no_self_disclosure_false_positive(void) {
    /* A normal Seth reply that merely contains "ai" inside a word must NOT
     * trip the flag (word-boundary discipline). */
    hu_shape_result_t r;
    memset(&r, 0, sizeof(r));
    const char *resp = "wait what time again";
    HU_ASSERT_EQ(hu_shape_classify(resp, strlen(resp), HU_SHAPE_CHANNEL_IMESSAGE, &r), HU_OK);
    HU_ASSERT_TRUE((r.fail_flags & HU_SHAPE_FAIL_AI_SELF_DISCLOSURE) == 0);
    HU_ASSERT_EQ((int)r.passed, 1);
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

    /* Task 11 (AC-10) — per-contact learned length caps. */
    HU_RUN_TEST(test_shape_close_contact_with_learned_cap_exceeds_universal);
    HU_RUN_TEST(test_shape_close_contact_with_learned_cap_bullet_list_still_fails);
    HU_RUN_TEST(test_shape_without_learned_cap_reverts_to_universal);
    HU_RUN_TEST(test_shape_non_close_relationship_ignores_learned_cap);

    /* 2026-05-29 — AI self-disclosure / persona-break. */
    HU_RUN_TEST(test_shape_ai_self_disclosure_fails_imessage);
    HU_RUN_TEST(test_shape_ai_no_access_disclaimer_fails_imessage);
    HU_RUN_TEST(test_shape_ai_self_disclosure_allowed_on_email);
    HU_RUN_TEST(test_shape_no_self_disclosure_false_positive);
    HU_RUN_TEST(test_shape_trailing_question_is_flagged);
    HU_RUN_TEST(test_shape_trailing_question_does_not_fail_the_response);
    HU_RUN_TEST(test_shape_no_trailing_question_not_flagged);
    HU_RUN_TEST(test_shape_bare_question_mark_not_flagged);
}
