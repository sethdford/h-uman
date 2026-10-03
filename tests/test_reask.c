/* tests/test_reask.c — contract tests for hu_agent_history_is_reask
 * (src/agent/turn/reask.c), the re-ask predicate hu_agent_turn feeds into the
 * "helpfulness" value. Histories are laid out as the turn leaves them: the
 * current user message is already in history, followed by its reply (and, on
 * a tool turn, by tool entries). */
#include "human/agent/reask.h"
#include "test_framework.h"
#include <string.h>

#define RK_MAX 16

typedef struct rk_hist {
    hu_owned_message_t m[RK_MAX];
    size_t n;
} rk_hist_t;

static void rk_add(rk_hist_t *h, hu_role_t role, const char *text) {
    if (h->n >= RK_MAX)
        return;
    hu_owned_message_t *m = &h->m[h->n++];
    memset(m, 0, sizeof(*m));
    m->role = role;
    m->content = (char *)text;
    m->content_len = strlen(text);
}

/* `pairs` earlier user/assistant exchanges drawn from k_chat, then the current
 * message and its reply. */
static const char *const k_chat[] = {
    "how was the drive up north",      "long but fine",
    "did you see the game last night", "yeah, wild ending",
    "are we still on for friday",      "yep, seven works",
    "can you send me that recipe",     "sent it over",
};

static void rk_conversation(rk_hist_t *h, size_t prior, const char *msg) {
    memset(h, 0, sizeof(*h));
    for (size_t i = 0; i < prior; i++)
        rk_add(h, (i % 2) ? HU_ROLE_ASSISTANT : HU_ROLE_USER, k_chat[i]);
    rk_add(h, HU_ROLE_USER, msg);
    rk_add(h, HU_ROLE_ASSISTANT, "ok.");
}

static bool rk_is_reask(const rk_hist_t *h, const char *msg) {
    return hu_agent_history_is_reask(h->m, h->n, msg, strlen(msg));
}

static const char k_new[] = "what time does the hardware store close";

/* The old inline scan began at the current message and matched it against
 * itself, so this returned true on every long-enough message. */
static void reask_new_question_in_a_long_history_is_not_a_reask(void) {
    rk_hist_t h;
    rk_conversation(&h, 8, k_new);
    HU_ASSERT_EQ(h.n, 10);
    HU_ASSERT_FALSE(rk_is_reask(&h, k_new));
}

static void reask_repeating_an_earlier_question_is_a_reask(void) {
    rk_hist_t h;
    rk_conversation(&h, 8, "are we still on for friday?");
    HU_ASSERT_TRUE(rk_is_reask(&h, "are we still on for friday?"));
}

/* N = 4..7: the window floor once wrapped on size_t and the scan never ran. */
static void reask_short_histories_detect_a_reask_and_ignore_a_new_question(void) {
    for (size_t prior = 2; prior <= 5; prior++) {
        rk_hist_t h;
        rk_conversation(&h, prior, "how was the drive up north?");
        HU_ASSERT_EQ(h.n, prior + 2);
        HU_ASSERT_TRUE(rk_is_reask(&h, "how was the drive up north?"));
        rk_conversation(&h, prior, k_new);
        HU_ASSERT_FALSE(rk_is_reask(&h, k_new));
    }
}

static void reask_needs_at_least_four_history_entries(void) {
    rk_hist_t h;
    rk_conversation(&h, 1, "how was the drive up north?");
    HU_ASSERT_EQ(h.n, 3);
    HU_ASSERT_FALSE(rk_is_reask(&h, "how was the drive up north?"));
}

static void reask_ignores_messages_of_ten_chars_or_fewer(void) {
    rk_hist_t h;
    memset(&h, 0, sizeof(h));
    rk_add(&h, HU_ROLE_USER, "0123456789");
    rk_add(&h, HU_ROLE_ASSISTANT, "sure");
    rk_add(&h, HU_ROLE_USER, "0123456789");
    rk_add(&h, HU_ROLE_ASSISTANT, "ok.");
    HU_ASSERT_FALSE(rk_is_reask(&h, "0123456789"));
}

/* 20-char messages: 14 matching positions is exactly 70% (not a re-ask),
 * 15 is 75% (a re-ask). */
static void reask_similarity_threshold_is_strictly_above_seventy_percent(void) {
    static const char msg[] = "abcdefghijklmnopqrst";
    rk_hist_t h;
    memset(&h, 0, sizeof(h));
    rk_add(&h, HU_ROLE_USER, "abcdefghijklmnXXXXXX"); /* 14/20 */
    rk_add(&h, HU_ROLE_ASSISTANT, "sure");
    rk_add(&h, HU_ROLE_USER, msg);
    rk_add(&h, HU_ROLE_ASSISTANT, "ok.");
    HU_ASSERT_FALSE(rk_is_reask(&h, msg));
    h.m[0].content = (char *)"abcdefghijklmnoXXXXX"; /* 15/20 */
    HU_ASSERT_TRUE(rk_is_reask(&h, msg));
}

/* On a tool turn the current message is not at history_count - 2; it is
 * found by content, and only an OLDER identical message counts. */
static void reask_finds_the_current_message_behind_tool_entries(void) {
    static const char msg[] = "list my saved project notes";
    rk_hist_t h;
    memset(&h, 0, sizeof(h));
    rk_add(&h, HU_ROLE_USER, "hey there, quick question");
    rk_add(&h, HU_ROLE_ASSISTANT, "go ahead");
    rk_add(&h, HU_ROLE_USER, msg);
    rk_add(&h, HU_ROLE_ASSISTANT, "");
    rk_add(&h, HU_ROLE_TOOL, "listed 2 items: alpha, beta");
    rk_add(&h, HU_ROLE_ASSISTANT, "alpha and beta");
    HU_ASSERT_FALSE(rk_is_reask(&h, msg));
    h.m[0].content = (char *)msg;
    h.m[0].content_len = strlen(msg);
    HU_ASSERT_TRUE(rk_is_reask(&h, msg));
}

/* Only the last 8 entries are scanned: a repeat 9 entries back is too old. */
static void reask_ignores_a_repeat_outside_the_window(void) {
    static const char msg[] = "how was the drive up north?";
    rk_hist_t h;
    memset(&h, 0, sizeof(h));
    rk_add(&h, HU_ROLE_USER, msg);
    for (size_t i = 1; i < 8; i++)
        rk_add(&h, (i % 2) ? HU_ROLE_ASSISTANT : HU_ROLE_USER, k_chat[i]);
    rk_add(&h, HU_ROLE_USER, msg);
    rk_add(&h, HU_ROLE_ASSISTANT, "ok.");
    HU_ASSERT_EQ(h.n, 10);
    HU_ASSERT_FALSE(rk_is_reask(&h, msg));
    h.m[2].content = (char *)msg; /* 8 entries back: inside the window */
    h.m[2].content_len = strlen(msg);
    HU_ASSERT_TRUE(rk_is_reask(&h, msg));
}

static void reask_rejects_null_inputs(void) {
    rk_hist_t h;
    rk_conversation(&h, 4, k_new);
    HU_ASSERT_FALSE(hu_agent_history_is_reask(NULL, 6, k_new, strlen(k_new)));
    HU_ASSERT_FALSE(hu_agent_history_is_reask(h.m, h.n, NULL, 0));
}

void run_reask_tests(void) {
    HU_TEST_SUITE("Reask");
    HU_RUN_TEST(reask_new_question_in_a_long_history_is_not_a_reask);
    HU_RUN_TEST(reask_repeating_an_earlier_question_is_a_reask);
    HU_RUN_TEST(reask_short_histories_detect_a_reask_and_ignore_a_new_question);
    HU_RUN_TEST(reask_needs_at_least_four_history_entries);
    HU_RUN_TEST(reask_ignores_messages_of_ten_chars_or_fewer);
    HU_RUN_TEST(reask_similarity_threshold_is_strictly_above_seventy_percent);
    HU_RUN_TEST(reask_finds_the_current_message_behind_tool_entries);
    HU_RUN_TEST(reask_ignores_a_repeat_outside_the_window);
    HU_RUN_TEST(reask_rejects_null_inputs);
}
