/* Deterministic guards around the director's expressive choices (spec
 * 2026-09-28-expressive-imessage): the model proposes, C decides whether a
 * flourish is appropriate right now. */
#include "human/daemon/expressive.h"
#include "test_framework.h"

#include <string.h>

static bool somber(const char *s) {
    return hu_expressive_somber(s, strlen(s));
}

static void test_expressive_somber_moments(void) {
    HU_ASSERT_TRUE(somber("Grandpa passed away this morning"));
    HU_ASSERT_TRUE(somber("she's in the hospital again"));
    HU_ASSERT_TRUE(somber("we're getting a divorce"));
    HU_ASSERT_FALSE(somber("got the job!!"));
    HU_ASSERT_FALSE(somber("I'm dying laughing")); /* not grief */
}

static void test_expressive_effect_is_rare_and_never_somber(void) {
    const int64_t week = 7 * 86400;
    HU_ASSERT_TRUE(hu_expressive_effect_allowed("confetti", false, false, -1));
    HU_ASSERT_FALSE(hu_expressive_effect_allowed("confetti", true, false, -1));    /* grief */
    HU_ASSERT_FALSE(hu_expressive_effect_allowed("confetti", false, true, -1));    /* group */
    HU_ASSERT_FALSE(hu_expressive_effect_allowed("confetti", false, false, 3600)); /* recent */
    HU_ASSERT_TRUE(hu_expressive_effect_allowed("confetti", false, false, week));
    HU_ASSERT_FALSE(hu_expressive_effect_allowed("explode", false, false, -1)); /* unknown */
    HU_ASSERT_FALSE(hu_expressive_effect_allowed("", false, false, -1));
}

static void test_expressive_gif_is_for_close_casual_contacts(void) {
    HU_ASSERT_TRUE(hu_expressive_gif_allowed(false, false, "friend", -1));
    HU_ASSERT_TRUE(hu_expressive_gif_allowed(false, false, "sister", 90000));
    HU_ASSERT_FALSE(hu_expressive_gif_allowed(false, false, "sister", 3600)); /* one a day */
    HU_ASSERT_FALSE(hu_expressive_gif_allowed(true, false, "friend", -1));
    HU_ASSERT_FALSE(hu_expressive_gif_allowed(false, true, "friend", -1));
    HU_ASSERT_FALSE(hu_expressive_gif_allowed(false, false, "mother", -1));
    HU_ASSERT_FALSE(hu_expressive_gif_allowed(false, false, "professional_friend", -1));
}

/* What the director is told is possible this turn. */
static void test_expressive_situation_tells_the_director_what_is_possible(void) {
    char buf[256];
    HU_ASSERT_TRUE(hu_expressive_situation(buf, sizeof(buf), true, true, false) > 0);
    HU_ASSERT_STR_CONTAINS(buf, "voice memo: available");
    HU_ASSERT_STR_CONTAINS(buf, "effects and threaded replies: available");
    HU_ASSERT_TRUE(hu_expressive_situation(buf, sizeof(buf), false, false, true) > 0);
    HU_ASSERT_STR_CONTAINS(buf, "voice memo: not available");
    HU_ASSERT_STR_CONTAINS(buf, "effects and threaded replies: not available");
    HU_ASSERT_STR_CONTAINS(buf, "group chat");
}

/* The shadow line says what the director chose and what the guards would allow. */
static void test_expressive_shadow_line_carries_choice_and_verdict(void) {
    hu_director_result_t r;
    memset(&r, 0, sizeof(r));
    r.form = HU_DIR_FORM_TEXT;
    memcpy(r.effect, "confetti", 9);
    r.reply_to = true;
    char line[256];
    const char *grief = "Grandpa passed away this morning";
    HU_ASSERT_TRUE(hu_expressive_shadow_line(&r, grief, strlen(grief), false, "sister", line,
                                             sizeof(line)) > 0);
    HU_ASSERT_STR_CONTAINS(line, "form=text");
    HU_ASSERT_STR_CONTAINS(line, "effect=confetti(blocked)");
    HU_ASSERT_STR_CONTAINS(line, "reply_to=1");
    const char *news = "I got the job!!";
    (void)hu_expressive_shadow_line(&r, news, strlen(news), false, "sister", line, sizeof(line));
    HU_ASSERT_STR_CONTAINS(line, "effect=confetti(ok)");
}

void run_daemon_expressive_tests(void) {
    HU_TEST_SUITE("daemon expressive");
    HU_RUN_TEST(test_expressive_somber_moments);
    HU_RUN_TEST(test_expressive_effect_is_rare_and_never_somber);
    HU_RUN_TEST(test_expressive_gif_is_for_close_casual_contacts);
    HU_RUN_TEST(test_expressive_situation_tells_the_director_what_is_possible);
    HU_RUN_TEST(test_expressive_shadow_line_carries_choice_and_verdict);
}
