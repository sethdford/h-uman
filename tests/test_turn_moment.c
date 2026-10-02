/* DEF-3 (2026-10-01): the moment cue hu_agent_turn puts in every reply's prompt
 * (src/agent/turn_moment.c). It was composed with NULL history and "never"
 * timestamps, so every reply 05:30-11:00 said "greet for a fresh morning" and
 * every reply 00:00-05:30 "acknowledge the late-hour gap", mid-conversation or
 * not. These tests pin that a greeting cue needs a REAL gap in the thread. */
#include "human/agent.h"
#include "human/agent/turn_moment.h"
#include "human/persona.h"
#include "test_framework.h"
#include <stdio.h>
#include <string.h>
#include <time.h>

#define GREET_MORNING "greet for a fresh morning"
#define ACK_LATE      "acknowledge the late-hour gap"

/* Local wall-clock time on a fixed, DST-free day (2026-03-04), so the phase
 * the moment layer computes from localtime is the one the test names. */
static int64_t at(int day_offset, int hour, int min) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = 126;
    tm.tm_mon = 2;
    tm.tm_mday = 4 + day_offset;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

static void entry(hu_channel_history_entry_t *e, bool from_me, int64_t ts, const char *text) {
    memset(e, 0, sizeof(*e));
    e->from_me = from_me;
    snprintf(e->text, sizeof(e->text), "%s", text);
    time_t t = (time_t)ts;
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(e->timestamp, sizeof(e->timestamp), "%Y-%m-%d %H:%M:%S", &lt);
}

static hu_persona_t s_persona; /* zeroed: presence only, like a loaded persona */
#define PERSONA ((const struct hu_persona_t *)&s_persona)

static void turn_moment_without_a_thread_renders_no_cue(void) {
    char buf[512];
    size_t n = hu_turn_moment_render_entries(PERSONA, NULL, NULL, 0, at(0, 8, 30), buf, sizeof buf);
    HU_ASSERT_EQ(n, 0u);
    HU_ASSERT_NULL(strstr(buf, GREET_MORNING));
    n = hu_turn_moment_render_entries(PERSONA, NULL, NULL, 0, at(0, 1, 30), buf, sizeof buf);
    HU_ASSERT_EQ(n, 0u);
    HU_ASSERT_NULL(strstr(buf, ACK_LATE));
}

static void turn_moment_morning_reply_mid_conversation_does_not_greet(void) {
    hu_channel_history_entry_t h[3];
    entry(&h[0], false, at(0, 8, 10), "you up");
    entry(&h[1], true, at(0, 8, 12), "yeah barely");
    entry(&h[2], false, at(0, 8, 29), "what time is the game");
    char buf[512];
    size_t n = hu_turn_moment_render_entries(PERSONA, NULL, h, 3, at(0, 8, 30), buf, sizeof buf);
    HU_ASSERT_GT(n, 0u);
    HU_ASSERT_NULL(strstr(buf, GREET_MORNING));
    HU_ASSERT_NOT_NULL(strstr(buf, "no greeting"));
}

static void turn_moment_first_message_after_the_night_greets(void) {
    hu_channel_history_entry_t h[3];
    entry(&h[0], true, at(-1, 21, 50), "night!");
    entry(&h[1], false, at(-1, 21, 55), "night");
    entry(&h[2], false, at(0, 8, 29), "morning");
    char buf[512];
    size_t n = hu_turn_moment_render_entries(PERSONA, NULL, h, 3, at(0, 8, 30), buf, sizeof buf);
    HU_ASSERT_GT(n, 0u);
    HU_ASSERT_NOT_NULL(strstr(buf, GREET_MORNING));
}

static void turn_moment_first_message_in_the_conversation_greets(void) {
    hu_channel_history_entry_t h[1];
    entry(&h[0], false, at(0, 8, 29), "hey");
    char buf[512];
    size_t n = hu_turn_moment_render_entries(PERSONA, NULL, h, 1, at(0, 8, 30), buf, sizeof buf);
    HU_ASSERT_GT(n, 0u);
    HU_ASSERT_NOT_NULL(strstr(buf, GREET_MORNING));
}

static void turn_moment_late_night_live_thread_does_not_acknowledge_a_gap(void) {
    hu_channel_history_entry_t h[3];
    entry(&h[0], false, at(0, 1, 10), "cant sleep");
    entry(&h[1], true, at(0, 1, 12), "same");
    entry(&h[2], false, at(0, 1, 29), "what are you watching");
    char buf[512];
    size_t n = hu_turn_moment_render_entries(PERSONA, NULL, h, 3, at(0, 1, 30), buf, sizeof buf);
    HU_ASSERT_GT(n, 0u);
    HU_ASSERT_NULL(strstr(buf, ACK_LATE));
}

static void turn_moment_late_night_message_after_a_gap_acknowledges_it(void) {
    hu_channel_history_entry_t h[2];
    entry(&h[0], true, at(-1, 18, 0), "see you then");
    entry(&h[1], false, at(0, 1, 29), "you awake?");
    char buf[512];
    size_t n = hu_turn_moment_render_entries(PERSONA, NULL, h, 2, at(0, 1, 30), buf, sizeof buf);
    HU_ASSERT_GT(n, 0u);
    HU_ASSERT_NOT_NULL(strstr(buf, ACK_LATE));
}

/* An earlier unanswered message is not part of the burst being answered: the
 * gap is measured to it, not past it. */
static void turn_moment_burst_is_only_the_recent_inbound_run(void) {
    hu_channel_history_entry_t h[3];
    entry(&h[0], false, at(0, 8, 0), "hey are you around");
    entry(&h[1], false, at(0, 8, 25), "nvm found it");
    entry(&h[2], false, at(0, 8, 29), "thanks anyway");
    char buf[512];
    size_t n = hu_turn_moment_render_entries(PERSONA, NULL, h, 3, at(0, 8, 30), buf, sizeof buf);
    HU_ASSERT_GT(n, 0u);
    HU_ASSERT_NULL(strstr(buf, GREET_MORNING));
}

/* The night sign-off cue reads the message being answered (the burst), not
 * the previous exchange: "night!" after a gap matches it. */
static void turn_moment_night_signoff_after_a_gap_reads_the_current_burst(void) {
    hu_channel_history_entry_t h[3];
    entry(&h[0], false, at(0, 15, 0), "running late, see you at the game");
    entry(&h[1], true, at(0, 15, 2), "all good");
    entry(&h[2], false, at(0, 22, 29), "night! talk tomorrow");
    char buf[512];
    size_t n = hu_turn_moment_render_entries(PERSONA, NULL, h, 3, at(0, 22, 30), buf, sizeof buf);
    HU_ASSERT_GT(n, 0u);
    HU_ASSERT_NOT_NULL(strstr(buf, "match the night sign-off"));
}

/* The turn's call reads the thread the daemon set on the agent. */
static void turn_moment_agent_call_uses_the_turn_thread(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.persona = &s_persona;
    char buf[512];
    HU_ASSERT_EQ(hu_turn_moment_render(&agent, at(0, 8, 30), buf, sizeof buf), 0u);

    hu_channel_history_entry_t h[3];
    entry(&h[0], false, at(0, 8, 10), "you up");
    entry(&h[1], true, at(0, 8, 12), "yeah");
    entry(&h[2], false, at(0, 8, 29), "ok cool");
    agent.ab_history_entries = h;
    agent.ab_history_count = 3;
    HU_ASSERT_GT(hu_turn_moment_render(&agent, at(0, 8, 30), buf, sizeof buf), 0u);
    HU_ASSERT_NULL(strstr(buf, GREET_MORNING));
    HU_ASSERT_NOT_NULL(strstr(buf, "Same thread"));
}

void run_turn_moment_tests(void) {
    HU_TEST_SUITE("turn_moment");
    HU_RUN_TEST(turn_moment_without_a_thread_renders_no_cue);
    HU_RUN_TEST(turn_moment_morning_reply_mid_conversation_does_not_greet);
    HU_RUN_TEST(turn_moment_first_message_after_the_night_greets);
    HU_RUN_TEST(turn_moment_first_message_in_the_conversation_greets);
    HU_RUN_TEST(turn_moment_late_night_live_thread_does_not_acknowledge_a_gap);
    HU_RUN_TEST(turn_moment_late_night_message_after_a_gap_acknowledges_it);
    HU_RUN_TEST(turn_moment_burst_is_only_the_recent_inbound_run);
    HU_RUN_TEST(turn_moment_agent_call_uses_the_turn_thread);
    HU_RUN_TEST(turn_moment_night_signoff_after_a_gap_reads_the_current_burst);
}
