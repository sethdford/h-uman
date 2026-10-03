/* The scene director's one-line decision (spec 2026-09-28-expressive-imessage,
 * Phase 2): today's fields keep their meaning; the new ones describe the whole
 * form of the response and are read only from the part before `direction:`,
 * which is free text. */
#include "human/daemon/director.h"
#include "test_framework.h"

#include <stdlib.h>
#include <string.h>

static void classify_comfort_response_type_returns_space_for_greeting(void) {
    const char *msg = "hey, how are you?";
    char out_type[16];
    hu_daemon_classify_comfort_response_type(msg, strlen(msg), out_type, sizeof(out_type));
    HU_ASSERT_STR_EQ(out_type, "space");
}

static void classify_comfort_response_type_returns_empathy_for_apology(void) {
    const char *msg = "I'm so sorry you're going through that";
    char out_type[16];
    hu_daemon_classify_comfort_response_type(msg, strlen(msg), out_type, sizeof(out_type));
    HU_ASSERT_STR_EQ(out_type, "empathy");
}

static hu_director_result_t parse(const char *raw) {
    hu_director_result_t r;
    hu_daemon_parse_director_result(raw, strlen(raw), &r);
    return r;
}

static void test_director_parse_keeps_todays_fields(void) {
    hu_director_result_t r = parse("action:tapback|delay_s:4|reaction:haha|direction:light");
    HU_ASSERT_EQ((int)r.action, (int)DIR_TAPBACK);
    HU_ASSERT_EQ((int)r.form, (int)HU_DIR_FORM_TAPBACK);
    HU_ASSERT_EQ(r.delay_s, 4u);
    HU_ASSERT_EQ((int)r.reaction, (int)HU_REACTION_HAHA);
    HU_ASSERT_STR_EQ(r.direction, "light");
    HU_ASSERT_STR_EQ(r.effect, "");
    HU_ASSERT_FALSE(r.reply_to);
}

/* voice and gif are forms the executor does not act on yet: they run as text,
 * so a SHADOW deployment changes nothing that is sent. */
static void test_director_parse_new_forms_run_as_text(void) {
    hu_director_result_t v = parse("action:voice|delay_s:20|direction:warm, a few thoughts");
    HU_ASSERT_EQ((int)v.form, (int)HU_DIR_FORM_VOICE);
    HU_ASSERT_EQ((int)v.action, (int)DIR_TEXT);
    hu_director_result_t g = parse("action:gif|gif:happy dance|direction:playful");
    HU_ASSERT_EQ((int)g.form, (int)HU_DIR_FORM_GIF);
    HU_ASSERT_EQ((int)g.action, (int)DIR_TEXT);
    HU_ASSERT_STR_EQ(g.gif_query, "happy dance");
}

static void test_director_parse_effect_and_thread(void) {
    hu_director_result_t r = parse("action:text|effect:confetti|reply_to:true|direction:hype");
    HU_ASSERT_STR_EQ(r.effect, "confetti");
    HU_ASSERT_TRUE(r.reply_to);
    hu_director_result_t bad = parse("action:text|effect:explode|direction:x");
    HU_ASSERT_STR_EQ(bad.effect, ""); /* not an id imsg knows */
}

/* A tone cue that happens to mention the fields must not be read as them. */
static void test_director_parse_ignores_fields_inside_direction(void) {
    hu_director_result_t r =
        parse("action:text|direction:no effect:confetti here, reply_to:true is not a flag");
    HU_ASSERT_STR_EQ(r.effect, "");
    HU_ASSERT_FALSE(r.reply_to);
    HU_ASSERT_STR_EQ(r.gif_query, "");
}

/* HU_DIRECTOR_FORMS off and shadow: the prompt is exactly today's. Shadow used
 * to append the forms too, and since the live decision comes from that same
 * call, "shadow" moved real choices: tapback-only decisions on real contacts
 * fell from 30-45% of turns (09-21..27) to 0-10% after the 09-28 deploy,
 * replaced by forms that run as plain text. Live: it teaches the new forms. */
static void test_director_prompt_forms_block_follows_the_gate(void) {
    static char buf[16384];
    unsetenv("HU_DIRECTOR_FORMS");
    size_t off = hu_daemon_director_system_prompt(buf, sizeof(buf));
    HU_ASSERT_TRUE(off > 0);
    HU_ASSERT_STR_NOT_CONTAINS(buf, "action:voice");
    setenv("HU_DIRECTOR_FORMS", "shadow", 1);
    size_t shadow = hu_daemon_director_system_prompt(buf, sizeof(buf));
    HU_ASSERT_EQ(shadow, off);
    HU_ASSERT_STR_NOT_CONTAINS(buf, "action:voice");
    setenv("HU_DIRECTOR_FORMS", "live", 1);
    size_t on = hu_daemon_director_system_prompt(buf, sizeof(buf));
    unsetenv("HU_DIRECTOR_FORMS");
    HU_ASSERT_TRUE(on > off);
    HU_ASSERT_STR_CONTAINS(buf, "action:voice");
    HU_ASSERT_STR_CONTAINS(buf, "effect:");
    HU_ASSERT_STR_CONTAINS(buf, "reply_to:true");
    HU_ASSERT_STR_CONTAINS(buf, "Never on sad news");
    HU_ASSERT_STR_CONTAINS(buf, "action:share");
}

/* Phase 5.1: sharing is a director choice with a kind and search words; the
 * reply still goes out as text, the share rides with it. */
static void test_director_parse_share(void) {
    hu_director_result_t r = parse("action:share|share:short|q:cat fail|direction:make her laugh");
    HU_ASSERT_EQ((int)r.form, (int)HU_DIR_FORM_SHARE);
    HU_ASSERT_EQ((int)r.action, (int)DIR_TEXT);
    HU_ASSERT_EQ((int)r.share, (int)HU_SHARE_SHORT);
    HU_ASSERT_STR_EQ(r.share_query, "cat fail");
    hu_director_result_t s = parse("action:share|share:song|q:sade smooth operator|direction:x");
    HU_ASSERT_EQ((int)s.share, (int)HU_SHARE_SONG);
    hu_director_result_t v = parse("action:share|share:saved|direction:x");
    HU_ASSERT_EQ((int)v.share, (int)HU_SHARE_SAVED);
    hu_director_result_t n = parse("action:text|direction:share:song q:nope");
    HU_ASSERT_EQ((int)n.share, (int)HU_SHARE_NONE);
}

/* "Vague unless known" (Seth, 2026-09-29): asked "how'd the big meeting go"
 * and "did you ever go to that concert" with nothing on record, the twin
 * answered "went better than expected actually" and "nah missed it". The
 * director's direction is the per-turn instruction the reply model follows,
 * so the rule lives there, on every prompt (forms gate off or on). */
static void test_director_prompt_never_directs_an_unknown_outcome(void) {
    static char buf[16384];
    unsetenv("HU_DIRECTOR_FORMS");
    (void)hu_daemon_director_system_prompt(buf, sizeof(buf));
    HU_ASSERT_STR_CONTAINS(buf, "Never direct an outcome");
    HU_ASSERT_STR_CONTAINS(buf, "don't say how it went");
}

void run_daemon_director_tests(void) {
    HU_TEST_SUITE("daemon_director");
    HU_RUN_TEST(test_director_prompt_never_directs_an_unknown_outcome);
    HU_RUN_TEST(classify_comfort_response_type_returns_space_for_greeting);
    HU_RUN_TEST(classify_comfort_response_type_returns_empathy_for_apology);
    HU_RUN_TEST(test_director_parse_keeps_todays_fields);
    HU_RUN_TEST(test_director_parse_new_forms_run_as_text);
    HU_RUN_TEST(test_director_parse_effect_and_thread);
    HU_RUN_TEST(test_director_parse_ignores_fields_inside_direction);
    HU_RUN_TEST(test_director_prompt_forms_block_follows_the_gate);
    HU_RUN_TEST(test_director_parse_share);
}
