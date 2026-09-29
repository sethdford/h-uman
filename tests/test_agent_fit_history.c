/* hu_agent_internal_fit_history (src/agent/agent.c): the request's message
 * history, fitted before it goes to the model. Owner self-tests (spec
 * 2026-09-28, Phase 5) cap it to the last few messages so a thread full of test
 * traffic does not confuse the reply; everyone else keeps the byte budget. */
#include "human/agent.h"
#include "human/context.h"
#include "test_framework.h"

#include <string.h>

size_t hu_agent_internal_fit_history(const hu_agent_t *agent, hu_chat_message_t *msgs,
                                     size_t msgs_count);

static hu_chat_message_t g_msgs[12];

static size_t fill(size_t history, size_t each_len) {
    static char body[12][6000];
    memset(g_msgs, 0, sizeof(g_msgs));
    g_msgs[0].role = HU_ROLE_SYSTEM;
    g_msgs[0].content = "system";
    g_msgs[0].content_len = 6;
    for (size_t i = 1; i <= history + 1; i++) {
        memset(body[i], 'a' + (int)(i % 26), each_len);
        body[i][each_len] = '\0';
        g_msgs[i].role = (i % 2) ? HU_ROLE_USER : HU_ROLE_ASSISTANT;
        g_msgs[i].content = body[i];
        g_msgs[i].content_len = each_len;
    }
    return history + 2; /* system + history + the current message */
}

static void test_fit_history_self_test_keeps_the_last_few(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    size_t n = fill(9, 10);
    const char *last = g_msgs[n - 1].content;
    const char *keep_first = g_msgs[n - 1 - 4].content; /* 4 prior messages kept */
    agent.history_msg_cap = 4;
    size_t out = hu_agent_internal_fit_history(&agent, g_msgs, n);
    HU_ASSERT_EQ(out, 6u); /* system + 4 prior + current */
    HU_ASSERT_TRUE(g_msgs[0].role == HU_ROLE_SYSTEM);
    HU_ASSERT_TRUE(g_msgs[1].content == keep_first);
    HU_ASSERT_TRUE(g_msgs[out - 1].content == last); /* the current message stays */
}

static void test_fit_history_uncapped_keeps_everything_under_budget(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    size_t n = fill(9, 10);
    HU_ASSERT_EQ(hu_agent_internal_fit_history(&agent, g_msgs, n), n);
}

static void test_fit_history_byte_budget_drops_oldest(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    size_t n = fill(9, 5000); /* ~50 KB of history against the 20 KB budget */
    const char *last = g_msgs[n - 1].content;
    size_t out = hu_agent_internal_fit_history(&agent, g_msgs, n);
    HU_ASSERT_TRUE(out < n);
    HU_ASSERT_TRUE(g_msgs[out - 1].content == last);
    size_t total = 0;
    for (size_t i = 0; i < out; i++)
        total += hu_chat_message_estimate_bytes(&g_msgs[i]);
    HU_ASSERT_TRUE(total <= 20 * 1024);
}

void run_agent_fit_history_tests(void) {
    HU_TEST_SUITE("agent fit history");
    HU_RUN_TEST(test_fit_history_self_test_keeps_the_last_few);
    HU_RUN_TEST(test_fit_history_uncapped_keeps_everything_under_budget);
    HU_RUN_TEST(test_fit_history_byte_budget_drops_oldest);
}
