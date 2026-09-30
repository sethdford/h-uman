/* Deterministic guards around the director's expressive choices (spec
 * 2026-09-28-expressive-imessage): the model proposes, C decides whether a
 * flourish is appropriate right now. */
#include "human/daemon/expressive.h"
#include "human/persona.h"
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
    HU_ASSERT_TRUE(hu_expressive_situation(buf, sizeof(buf), true, true, false, true) > 0);
    HU_ASSERT_STR_CONTAINS(buf, "voice memo: available");
    HU_ASSERT_STR_CONTAINS(buf, "effects and threaded replies: available");
    HU_ASSERT_TRUE(hu_expressive_situation(buf, sizeof(buf), false, false, true, false) > 0);
    HU_ASSERT_STR_CONTAINS(buf, "voice memo: not available");
    HU_ASSERT_STR_CONTAINS(buf, "effects and threaded replies: not available");
    HU_ASSERT_STR_CONTAINS(buf, "group chat");
    HU_ASSERT_STR_NOT_CONTAINS(buf, "saved");
    (void)hu_expressive_situation(buf, sizeof(buf), true, true, false, true);
    HU_ASSERT_STR_CONTAINS(buf, "Seth saved a link for them"); /* share:saved is on the table */
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
    r.form = HU_DIR_FORM_SHARE;
    r.share = HU_SHARE_SHORT;
    memcpy(r.share_query, "cat fail", 9);
    (void)hu_expressive_shadow_line(&r, news, strlen(news), false, "sister", line, sizeof(line));
    HU_ASSERT_STR_CONTAINS(line, "share=short(ok)");
}

/* Phase 5.1: at most one share a day per person, never somber, never groups. */
static void test_expressive_share_budget(void) {
    HU_ASSERT_TRUE(hu_expressive_share_allowed(false, false, -1));
    HU_ASSERT_FALSE(hu_expressive_share_allowed(false, false, 3600));
    HU_ASSERT_TRUE(hu_expressive_share_allowed(false, false, 90000));
    HU_ASSERT_FALSE(hu_expressive_share_allowed(true, false, -1));
    HU_ASSERT_FALSE(hu_expressive_share_allowed(false, true, -1));
}

static void test_expressive_share_medium(void) {
    HU_ASSERT_EQ((int)hu_expressive_share_medium(HU_SHARE_SONG, false), (int)HU_INSPIRATION_MUSIC);
    HU_ASSERT_EQ((int)hu_expressive_share_medium(HU_SHARE_VIDEO, true),
                 (int)HU_INSPIRATION_YOUTUBE);
    HU_ASSERT_EQ((int)hu_expressive_share_medium(HU_SHARE_SHORT, true),
                 (int)HU_INSPIRATION_YOUTUBE);
    HU_ASSERT_EQ((int)hu_expressive_share_medium(HU_SHARE_SHORT, false), (int)HU_INSPIRATION_NONE);
    HU_ASSERT_EQ((int)hu_expressive_share_medium(HU_SHARE_SAVED, true), (int)HU_INSPIRATION_NONE);
}

/* One decider: a director share goes; with the director LIVE nothing else shares
 * (the old 5% dice stop); otherwise today's dice. Saved links have their own path. */
static void test_expressive_share_should_go(void) {
    hu_director_result_t d;
    memset(&d, 0, sizeof(d));
    hu_share_kind_t k = HU_SHARE_NONE;
    d.form = HU_DIR_FORM_SHARE;
    d.share = HU_SHARE_SHORT;
    HU_ASSERT_TRUE(hu_expressive_share_should_go(&d, true, false, &k));
    HU_ASSERT_EQ((int)k, (int)HU_SHARE_SHORT);
    HU_ASSERT_FALSE(hu_expressive_share_should_go(NULL, true, true, &k)); /* live: no dice */
    HU_ASSERT_TRUE(hu_expressive_share_should_go(NULL, false, true, &k)); /* today's dice */
    HU_ASSERT_EQ((int)k, (int)HU_SHARE_NONE);
    HU_ASSERT_FALSE(hu_expressive_share_should_go(NULL, false, false, &k));
    d.share = HU_SHARE_SAVED;
    HU_ASSERT_FALSE(hu_expressive_share_should_go(&d, true, true, &k)); /* queue path */
    d.form = HU_DIR_FORM_TEXT; /* a share kind without the share form is not a share */
    d.share = HU_SHARE_SONG;
    HU_ASSERT_FALSE(hu_expressive_share_should_go(&d, true, false, &k));
}

/* The one door a director share goes through: only LIVE (a SHADOW choice is
 * logged, never sent), and only past the guards — not somber, not a group,
 * once a day per contact. */
static void test_expressive_share_gate(void) {
    hu_director_result_t d;
    memset(&d, 0, sizeof(d));
    d.form = HU_DIR_FORM_SHARE;
    d.share = HU_SHARE_SONG;
    const char *hi = "have you heard the new Beach House album";
    const char *grief = "Grandpa passed away this morning";
    HU_ASSERT_NULL(hu_expressive_share_gate(&d, true, false, hi, strlen(hi), false, "+15550000071",
                                            12, 1000)); /* shadow */
    HU_ASSERT_TRUE(hu_expressive_share_gate(&d, true, true, hi, strlen(hi), false, "+15550000071",
                                            12, 1000) == &d);
    HU_ASSERT_NULL(hu_expressive_share_gate(&d, true, true, hi, strlen(hi), false, "+15550000071",
                                            12, 5000)); /* same day */
    HU_ASSERT_TRUE(hu_expressive_share_gate(&d, true, true, hi, strlen(hi), false, "+15550000071",
                                            12, 1000 + 86400) == &d); /* next day */
    HU_ASSERT_NULL(hu_expressive_share_gate(&d, true, true, grief, strlen(grief), false,
                                            "+15550000072", 12, 1000));
    HU_ASSERT_NULL(hu_expressive_share_gate(&d, true, true, hi, strlen(hi), true, "+15550000073",
                                            12, 1000)); /* group */
    HU_ASSERT_NULL(hu_expressive_share_gate(&d, false, true, hi, strlen(hi), false, "+15550000074",
                                            12, 1000)); /* no valid director result */
    d.form = HU_DIR_FORM_TEXT;
    HU_ASSERT_NULL(
        hu_expressive_share_gate(&d, true, true, hi, strlen(hi), false, "+15550000075", 12, 1000));
}

/* Unknown-event guard (Seth, 2026-09-30): a question that presupposes an
 * event in Seth's life, when the thread doesn't mention it, gets an invented
 * outcome from the model ("how'd the big meeting go" -> "went better than
 * expected actually") even with prompt rules. A code check catches it. */
static bool ue(const char *msg, const char *hist0, char *topic, size_t cap) {
    hu_channel_history_entry_t h[2];
    memset(h, 0, sizeof(h));
    size_t n = 0;
    if (hist0) {
        snprintf(h[0].text, sizeof(h[0].text), "%s", hist0);
        n = 1;
    }
    return hu_expressive_unknown_event(msg, strlen(msg), h, n, topic, cap);
}

static void test_unknown_event_catches_presupposed_outcomes(void) {
    char t[64];
    HU_ASSERT_TRUE(ue("how'd the big meeting go", NULL, t, sizeof(t)));
    HU_ASSERT_STR_EQ(t, "big meeting");
    HU_ASSERT_TRUE(ue("How did the interview go?", NULL, t, sizeof(t)));
    HU_ASSERT_STR_EQ(t, "interview");
    HU_ASSERT_TRUE(ue("did you ever go to that concert", NULL, t, sizeof(t)));
    HU_ASSERT_STR_EQ(t, "concert");
    HU_ASSERT_TRUE(ue("did you make it to the game?", NULL, t, sizeof(t)));
    HU_ASSERT_STR_EQ(t, "game");
    HU_ASSERT_TRUE(ue("how's ryan settling into the new place", NULL, t, sizeof(t)));
    HU_ASSERT_STR_EQ(t, "ryan");
    HU_ASSERT_TRUE(ue("how\xe2\x80\x99"
                      "d the trip go",
                      NULL, t, sizeof(t))); /* curly quote */
    HU_ASSERT_STR_EQ(t, "trip");
}

/* The loaded history already holds the message being answered (live
 * 2026-09-30 05:30: the guard never fired because "dentist" was "known" from
 * the question itself). The question does not establish its own event. */
static void test_unknown_event_ignores_the_question_itself_in_history(void) {
    char t[64];
    HU_ASSERT_TRUE(
        ue("how'd the dentist appointment go", "how'd the dentist appointment go", t, sizeof(t)));
    HU_ASSERT_TRUE(ue("how'd the dentist appointment go", "#text how'd the dentist appointment go",
                      t, sizeof(t)));
    HU_ASSERT_STR_EQ(t, "dentist appointment");
}

static void test_unknown_event_passes_what_the_thread_established(void) {
    char t[64];
    HU_ASSERT_FALSE(
        ue("how'd the big meeting go", "big meeting with the board at 10 tomorrow", t, sizeof(t)));
    HU_ASSERT_FALSE(
        ue("did you go to that concert", "grabbed tickets for the concert friday", t, sizeof(t)));
}

/* Ordinary questions are not events; a pronoun points back into the thread. */
static void test_unknown_event_ignores_ordinary_questions(void) {
    char t[64];
    HU_ASSERT_FALSE(ue("how was your day", NULL, t, sizeof(t)));
    HU_ASSERT_FALSE(ue("how'd your weekend go", NULL, t, sizeof(t)));
    HU_ASSERT_FALSE(ue("how'd it go", NULL, t, sizeof(t)));
    HU_ASSERT_FALSE(ue("how's the weather", NULL, t, sizeof(t)));
    HU_ASSERT_FALSE(ue("how are you", NULL, t, sizeof(t)));
    HU_ASSERT_FALSE(ue("did you eat yet", NULL, t, sizeof(t)));
    HU_ASSERT_FALSE(ue("", NULL, t, sizeof(t)));
}

/* The gate contract: off (default) and shadow leave the director's direction
 * untouched; live replaces it. Each half alone could pass vacuously. */
static void test_unknown_event_guard_follows_its_gate(void) {
    const char *q = "how'd the big meeting go";
    hu_director_result_t d;
    memset(&d, 0, sizeof(d));
    snprintf(d.direction, sizeof(d.direction), "mention it was a long one");
    unsetenv("HU_UNKNOWN_EVENT_GUARD");
    hu_expressive_unknown_event_guard(&d, q, strlen(q), NULL, 0);
    HU_ASSERT_STR_EQ(d.direction, "mention it was a long one");
    setenv("HU_UNKNOWN_EVENT_GUARD", "shadow", 1);
    hu_expressive_unknown_event_guard(&d, q, strlen(q), NULL, 0);
    HU_ASSERT_STR_EQ(d.direction, "mention it was a long one");
    setenv("HU_UNKNOWN_EVENT_GUARD", "live", 1);
    hu_expressive_unknown_event_guard(&d, q, strlen(q), NULL, 0);
    unsetenv("HU_UNKNOWN_EVENT_GUARD");
    HU_ASSERT_STR_CONTAINS(d.direction, "don't say how the big meeting went");
}

static void test_unknown_event_direction_names_the_topic(void) {
    char d[256];
    hu_expressive_unknown_event_direction("big meeting", d, sizeof(d));
    HU_ASSERT_STR_CONTAINS(d, "don't say how");
    HU_ASSERT_STR_CONTAINS(d, "big meeting");
}

/* Self-test commands (Seth, 2026-09-29: "I do like testing things with myself
 * ... we should make this more robust"): from his own number, a #command
 * forces one behavior so he can validate it on his phone. */
static bool st(const char *s, hu_selftest_t *t) {
    return hu_selftest_parse(s, strlen(s), t);
}

static void test_selftest_commands(void) {
    hu_selftest_t t;
    HU_ASSERT_TRUE(st("#voice how was your day", &t));
    HU_ASSERT_EQ((int)t.form, (int)HU_DIR_FORM_VOICE);
    HU_ASSERT_EQ(t.consumed, 7u); /* "#voice " stripped before the turn */
    HU_ASSERT_TRUE(st("#share short cat fail", &t));
    HU_ASSERT_EQ((int)t.form, (int)HU_DIR_FORM_SHARE);
    HU_ASSERT_EQ((int)t.share, (int)HU_SHARE_SHORT);
    HU_ASSERT_STR_EQ(t.query, "cat fail");
    HU_ASSERT_TRUE(st("#effect confetti we did it", &t));
    HU_ASSERT_STR_EQ(t.effect, "confetti");
    HU_ASSERT_EQ(t.consumed, strlen("#effect confetti "));
    HU_ASSERT_TRUE(st("#tapback laugh", &t));
    HU_ASSERT_EQ((int)t.form, (int)HU_DIR_FORM_TAPBACK);
    HU_ASSERT_EQ((int)t.reaction, (int)HU_REACTION_HAHA);
    HU_ASSERT_TRUE(st("#gif happy dance", &t));
    HU_ASSERT_EQ((int)t.form, (int)HU_DIR_FORM_GIF);
    HU_ASSERT_STR_EQ(t.query, "happy dance");
    /* #text: a normal text reply (typing rhythm, pacing) on demand. */
    HU_ASSERT_TRUE(st("#text what did you do this morning", &t));
    HU_ASSERT_EQ((int)t.form, (int)HU_DIR_FORM_TEXT);
    HU_ASSERT_EQ(t.consumed, strlen("#text "));
    HU_ASSERT_STR_EQ(t.effect, "");
    HU_ASSERT_FALSE(st("#texting", &t));
    HU_ASSERT_FALSE(st("#effect explode hi", &t)); /* not an effect imsg knows */
    HU_ASSERT_FALSE(st("#share podcast x", &t));
    HU_ASSERT_FALSE(st("just #voice in the middle", &t));
    HU_ASSERT_FALSE(st("#voiceover", &t));
}

/* A self-test command becomes the director's choice for that turn. */
static void test_selftest_apply_overrides_the_director(void) {
    hu_director_result_t d;
    memset(&d, 0, sizeof(d));
    hu_selftest_t t;
    HU_ASSERT_TRUE(hu_selftest_parse("#tapback laugh", 14, &t));
    hu_expressive_selftest_apply(&t, &d);
    HU_ASSERT_EQ((int)d.action, (int)DIR_TAPBACK);
    HU_ASSERT_EQ((int)d.reaction, (int)HU_REACTION_HAHA);
    memset(&d, 0, sizeof(d));
    HU_ASSERT_TRUE(hu_selftest_parse("#share song beach house", 23, &t));
    hu_expressive_selftest_apply(&t, &d);
    HU_ASSERT_EQ((int)d.form, (int)HU_DIR_FORM_SHARE);
    HU_ASSERT_EQ((int)d.share, (int)HU_SHARE_SONG);
    HU_ASSERT_STR_EQ(d.share_query, "beach house");
    HU_ASSERT_EQ((int)d.action, (int)DIR_TEXT); /* the reply still goes */
    memset(&d, 0, sizeof(d));
    HU_ASSERT_TRUE(hu_selftest_parse("#effect lasers pew", 18, &t));
    hu_expressive_selftest_apply(&t, &d);
    HU_ASSERT_STR_EQ(d.effect, "lasers");
}

/* The director's effect, LIVE: allowed moments only, once a week per contact. */
static void test_expressive_effect_gate(void) {
    hu_director_result_t d;
    memset(&d, 0, sizeof(d));
    memcpy(d.effect, "confetti", 9);
    const char *news = "I got the job!!";
    char e[16];
    HU_ASSERT_FALSE(hu_expressive_effect_gate(&d, true, false, news, strlen(news), false,
                                              "+15550000081", 12, 1000, e, sizeof(e))); /* shadow */
    HU_ASSERT_TRUE(hu_expressive_effect_gate(&d, true, true, news, strlen(news), false,
                                             "+15550000081", 12, 1000, e, sizeof(e)));
    HU_ASSERT_STR_EQ(e, "confetti");
    HU_ASSERT_FALSE(hu_expressive_effect_gate(&d, true, true, news, strlen(news), false,
                                              "+15550000081", 12, 90000, e, sizeof(e))); /* week */
    HU_ASSERT_TRUE(hu_expressive_effect_gate(&d, true, true, news, strlen(news), false,
                                             "+15550000081", 12, 1000 + 7 * 86400, e, sizeof(e)));
    const char *grief = "Grandpa passed away";
    HU_ASSERT_FALSE(hu_expressive_effect_gate(&d, true, true, grief, strlen(grief), false,
                                              "+15550000082", 12, 1000, e, sizeof(e)));
}

/* A self-test from Seth's own number is answered at any hour (the 2-6 AM
 * late-night skip ate the first live #voice test, 2026-09-29 05:10). */
static void test_selftest_from_owner(void) {
    static hu_contact_profile_t cs[2];
    memset(cs, 0, sizeof(cs));
    cs[0].contact_id = "+15550000009";
    cs[0].relationship = "test";
    cs[1].contact_id = "+15550000001";
    cs[1].relationship = "sister";
    hu_persona_t p;
    memset(&p, 0, sizeof(p));
    p.contacts = cs;
    p.contacts_count = 2;
    const char *cmd = "#voice hey";
    HU_ASSERT_TRUE(hu_selftest_from_owner(&p, "+15550000009", 12, cmd, strlen(cmd)));
    HU_ASSERT_FALSE(hu_selftest_from_owner(&p, "+15550000001", 12, cmd, strlen(cmd)));
    const char *chat = "hey how are you";
    HU_ASSERT_FALSE(hu_selftest_from_owner(&p, "+15550000009", 12, chat, strlen(chat)));
    HU_ASSERT_FALSE(hu_selftest_from_owner(NULL, "+15550000009", 12, cmd, strlen(cmd)));
}

void run_daemon_expressive_tests(void) {
    HU_TEST_SUITE("daemon expressive");
    HU_RUN_TEST(test_expressive_somber_moments);
    HU_RUN_TEST(test_expressive_effect_is_rare_and_never_somber);
    HU_RUN_TEST(test_expressive_gif_is_for_close_casual_contacts);
    HU_RUN_TEST(test_expressive_situation_tells_the_director_what_is_possible);
    HU_RUN_TEST(test_expressive_shadow_line_carries_choice_and_verdict);
    HU_RUN_TEST(test_expressive_share_budget);
    HU_RUN_TEST(test_expressive_share_medium);
    HU_RUN_TEST(test_expressive_share_should_go);
    HU_RUN_TEST(test_expressive_share_gate);
    HU_RUN_TEST(test_selftest_commands);
    HU_RUN_TEST(test_unknown_event_catches_presupposed_outcomes);
    HU_RUN_TEST(test_unknown_event_ignores_the_question_itself_in_history);
    HU_RUN_TEST(test_unknown_event_passes_what_the_thread_established);
    HU_RUN_TEST(test_unknown_event_ignores_ordinary_questions);
    HU_RUN_TEST(test_unknown_event_direction_names_the_topic);
    HU_RUN_TEST(test_unknown_event_guard_follows_its_gate);
    HU_RUN_TEST(test_selftest_apply_overrides_the_director);
    HU_RUN_TEST(test_expressive_effect_gate);
    HU_RUN_TEST(test_selftest_from_owner);
}
