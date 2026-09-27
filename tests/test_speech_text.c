/* tests/test_speech_text.c
 *
 * F1 S2/S3: the spoken form of a reply (texting shorthand, stage directions,
 * emoji and URLs never reach TTS) and the drift guard that keeps a
 * rewrite-for-the-ear about HOW it is said, never WHAT. */
#include "human/tts/speech_text.h"
#include "test_framework.h"

#include <string.h>

static void expect_clean(const char *in, const char *want, bool want_cue) {
    char out[512];
    bool cue = false;
    size_t n = hu_speech_cleanup(in, strlen(in), out, sizeof(out), &cue);
    HU_ASSERT_EQ(n, strlen(want));
    HU_ASSERT_STR_EQ(out, want);
    HU_ASSERT_EQ(cue, want_cue);
}

static void test_speech_cleanup_expands_texting_shorthand(void) {
    expect_clean("yeah lol that sounds good, lmk when ur free tmrw",
                 "yeah that sounds good, let me know when you're free tomorrow", true);
    expect_clean("idk tbh, omw rn bc u asked",
                 "I don't know honestly, on my way right now because you asked", false);
    expect_clean("ngl ur mom is the best", "not gonna lie your mom is the best", false);
}

static void test_speech_cleanup_word_boundaries_only(void) {
    /* "rn" inside "turn", "u" inside "but", "lol" inside "lollipop" stay put. */
    expect_clean("turn it off but grab a lollipop", "turn it off but grab a lollipop", false);
}

static void test_speech_cleanup_drops_actions_keeps_emphasis(void) {
    expect_clean("*laughs* ok", "ok", false);
    expect_clean("that was *really* good", "that was really good", false);
    expect_clean("(sighs) fine", "fine", false);
    expect_clean("meet at the cafe (the one on 5th)", "meet at the cafe the one on 5th", false);
    expect_clean("[pause] sure", "sure", false);
    expect_clean("haha that's wild", "that's wild", true);
}

static void test_speech_cleanup_urls_and_emoji(void) {
    expect_clean("check this https://x.example/a ok", "check this ok", false);
    expect_clean("https://x.example/a", "I'll send you the link", false);
    expect_clean("sounds good \xF0\x9F\x91\x8D", "sounds good", false);
    char out[64];
    bool cue = false;
    HU_ASSERT_EQ(hu_speech_cleanup("\xF0\x9F\x91\x8D", 4, out, sizeof(out), &cue), 0);
}

/* Final review #5: bracket/quote bodies go through the same URL + emoji pass. */
static void test_speech_cleanup_strips_urls_and_emoji_inside_brackets(void) {
    expect_clean("more here (https://example.com/abc) ok", "more here ok", false);
    expect_clean("love it (so good \xF0\x9F\x98\x8D)", "love it so good", false);
    expect_clean("see:https://x.example/a ok", "see: ok", false);
    char out[128];
    const char *q = "look \"https://ex.example/a\"";
    (void)hu_speech_cleanup(q, strlen(q), out, sizeof(out), NULL);
    HU_ASSERT_STR_NOT_CONTAINS(out, "http");
}

/* Final review #4: a memo never carries the link, so a reply with one goes as text. */
static void test_speech_has_url_finds_links_anywhere(void) {
    HU_ASSERT_TRUE(hu_speech_has_url("https://x.example/a", 19));
    HU_ASSERT_TRUE(hu_speech_has_url("the place (www.example.com)", 27));
    HU_ASSERT_FALSE(hu_speech_has_url("meet at the cafe on 5th", 23));
    HU_ASSERT_FALSE(hu_speech_has_url(NULL, 0));
}

static void test_speech_cleanup_null_and_tiny_buffer_safe(void) {
    char out[8];
    bool cue = true;
    HU_ASSERT_EQ(hu_speech_cleanup(NULL, 0, out, sizeof(out), &cue), 0);
    HU_ASSERT_FALSE(cue);
    HU_ASSERT_EQ(hu_speech_cleanup("hello there friend", 18, out, sizeof(out), NULL),
                 strlen(out)); /* truncates, stays terminated */
    HU_ASSERT_TRUE(strlen(out) < sizeof(out));
}

static void expect_drift(const char *orig, const char *rew, hu_speech_drift_t want) {
    HU_ASSERT_EQ(hu_speech_drift_check(orig, strlen(orig), rew, strlen(rew)), want);
}

static void test_speech_drift_accepts_a_faithful_rewrite(void) {
    expect_drift("yeah that sounds good, let me know when you're free tomorrow",
                 "Yeah, that sounds good. Let me know when you're free tomorrow.",
                 HU_SPEECH_DRIFT_OK);
    expect_drift("i said i'd call you", "I said I'd call you.", HU_SPEECH_DRIFT_OK);
}

static void test_speech_drift_rejects_each_kind_of_drift(void) {
    expect_drift("see you tomorrow", "See you tomorrow at 7.", HU_SPEECH_DRIFT_NEW_NUMBER);
    expect_drift("tell her i said hi", "Tell her Sarah said hi.", HU_SPEECH_DRIFT_NEW_NAME);
    expect_drift("sounds good", "Sounds good?", HU_SPEECH_DRIFT_QUESTION);
    expect_drift("want to grab dinner?", "Grab dinner.", HU_SPEECH_DRIFT_QUESTION);
    expect_drift("that was a really long day and i am so tired of all of it honestly", "Long day.",
                 HU_SPEECH_DRIFT_LENGTH);
    expect_drift("sounds good to me", "Well, sounds good to me.", HU_SPEECH_DRIFT_BANNED);
    expect_drift("sounds good to me", "*smiles* sounds good to me", HU_SPEECH_DRIFT_BANNED);
    expect_drift("sounds good to me", "Good question, sounds good to me.", HU_SPEECH_DRIFT_BANNED);
}

/* Final review #3: facts a capital letter does not mark. */
static void test_speech_drift_rejects_spelled_numbers_and_dates(void) {
    expect_drift("ok see you tomorrow evening then", "Ok, see you at seven tomorrow evening then.",
                 HU_SPEECH_DRIFT_NEW_NUMBER);
    expect_drift("see you soon then ok", "see you monday then, ok.", HU_SPEECH_DRIFT_NEW_NUMBER);
    expect_drift("sounds good see you later", "Sounds good, see you tonight.",
                 HU_SPEECH_DRIFT_NEW_NUMBER);
    expect_drift("see you at seven on monday", "See you at seven on Monday.", HU_SPEECH_DRIFT_OK);
}

static void test_speech_drift_rejects_negation_flip(void) {
    expect_drift("i can make it tonight", "I can't make it tonight.", HU_SPEECH_DRIFT_NEGATION);
    expect_drift("i don't think so", "I think so.", HU_SPEECH_DRIFT_NEGATION);
    expect_drift("i don't know yet", "I don't know yet.", HU_SPEECH_DRIFT_OK);
    expect_drift("no worries at all", "No worries at all.", HU_SPEECH_DRIFT_OK);
}

/* Live preview: iPhone texts use U+2019 ("won’t"); the model writes ASCII. */
static void test_speech_drift_curly_apostrophe_is_an_apostrophe(void) {
    expect_drift("i won\xE2\x80\x99t be there much", "I won't be there much.", HU_SPEECH_DRIFT_OK);
    expect_drift("the airbnb option is great", "Yeah the Airbnb\xE2\x80\x99s great.",
                 HU_SPEECH_DRIFT_OK);
    expect_drift("i will be there", "I won\xE2\x80\x99t be there.", HU_SPEECH_DRIFT_NEGATION);
}

static void test_speech_drift_known_name_is_fine(void) {
    expect_drift("tell sarah i said hi", "Tell Sarah I said hi.", HU_SPEECH_DRIFT_OK);
    /* Live preview 2026-09-27: a possessive is the same name, not a new one. */
    expect_drift("the airbnb option is great", "Yeah the Airbnb's great.", HU_SPEECH_DRIFT_OK);
}

static void test_speech_drift_names_are_distinct(void) {
    HU_ASSERT_STR_EQ(hu_speech_drift_name(HU_SPEECH_DRIFT_OK), "ok");
    HU_ASSERT_STR_EQ(hu_speech_drift_name(HU_SPEECH_DRIFT_NEW_NUMBER), "new_number");
    HU_ASSERT_STR_EQ(hu_speech_drift_name(HU_SPEECH_DRIFT_BANNED), "banned");
}

void run_speech_text_tests(void) {
    HU_TEST_SUITE("speech text (F1 S2/S3)");
    HU_RUN_TEST(test_speech_cleanup_expands_texting_shorthand);
    HU_RUN_TEST(test_speech_cleanup_word_boundaries_only);
    HU_RUN_TEST(test_speech_cleanup_drops_actions_keeps_emphasis);
    HU_RUN_TEST(test_speech_cleanup_urls_and_emoji);
    HU_RUN_TEST(test_speech_cleanup_strips_urls_and_emoji_inside_brackets);
    HU_RUN_TEST(test_speech_has_url_finds_links_anywhere);
    HU_RUN_TEST(test_speech_cleanup_null_and_tiny_buffer_safe);
    HU_RUN_TEST(test_speech_drift_accepts_a_faithful_rewrite);
    HU_RUN_TEST(test_speech_drift_rejects_each_kind_of_drift);
    HU_RUN_TEST(test_speech_drift_rejects_spelled_numbers_and_dates);
    HU_RUN_TEST(test_speech_drift_rejects_negation_flip);
    HU_RUN_TEST(test_speech_drift_curly_apostrophe_is_an_apostrophe);
    HU_RUN_TEST(test_speech_drift_known_name_is_fine);
    HU_RUN_TEST(test_speech_drift_names_are_distinct);
}
