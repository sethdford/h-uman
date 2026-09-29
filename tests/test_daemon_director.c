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

/* HU_DIRECTOR_FORMS off: the prompt is exactly today's. On: it teaches the new
 * forms and their appropriateness rules. */
static void test_director_prompt_forms_block_follows_the_gate(void) {
    static char buf[16384];
    unsetenv("HU_DIRECTOR_FORMS");
    size_t off = hu_daemon_director_system_prompt(buf, sizeof(buf));
    HU_ASSERT_TRUE(off > 0);
    HU_ASSERT_STR_NOT_CONTAINS(buf, "action:voice");
    setenv("HU_DIRECTOR_FORMS", "shadow", 1);
    size_t on = hu_daemon_director_system_prompt(buf, sizeof(buf));
    unsetenv("HU_DIRECTOR_FORMS");
    HU_ASSERT_TRUE(on > off);
    HU_ASSERT_STR_CONTAINS(buf, "action:voice");
    HU_ASSERT_STR_CONTAINS(buf, "effect:");
    HU_ASSERT_STR_CONTAINS(buf, "reply_to:true");
    HU_ASSERT_STR_CONTAINS(buf, "Never on sad news");
}

void run_daemon_director_tests(void) {
    HU_TEST_SUITE("daemon_director");
    HU_RUN_TEST(classify_comfort_response_type_returns_space_for_greeting);
    HU_RUN_TEST(classify_comfort_response_type_returns_empathy_for_apology);
    HU_RUN_TEST(test_director_parse_keeps_todays_fields);
    HU_RUN_TEST(test_director_parse_new_forms_run_as_text);
    HU_RUN_TEST(test_director_parse_effect_and_thread);
    HU_RUN_TEST(test_director_parse_ignores_fields_inside_direction);
    HU_RUN_TEST(test_director_prompt_forms_block_follows_the_gate);
}
