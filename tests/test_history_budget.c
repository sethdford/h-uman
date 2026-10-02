/* HU_HISTORY_BUDGET (2026-10-01): the request history budget counts the
 * history only, not the system prompt. With the legacy policy a 22 KB system
 * prompt already spends the 20 KB budget, so every prior message is dropped
 * (msgs=2). Pins: OFF keeps the legacy result byte for byte, LIVE keeps the
 * history, LIVE never exceeds the configured total cap, the cap's env parse
 * is clamped, and the agent entry point honours the gate. */
#include "human/agent.h"
#include "human/agent/history_budget.h"
#include "human/context.h"
#include "test_framework.h"
#include <stdlib.h>
#include <string.h>

size_t hu_agent_internal_fit_history(const hu_agent_t *agent, hu_chat_message_t *msgs,
                                     size_t msgs_count);

#define HB_MAX 40
static hu_chat_message_t g_hb[HB_MAX];
static char g_sys[100 * 1024];
static char g_body[HB_MAX][2048];

/* msgs[0] = system of sys_len bytes, `prior` history messages of each_len,
 * then the current message (200 B). Returns the count. */
static size_t hb_fill(size_t sys_len, size_t prior, size_t each_len) {
    memset(g_hb, 0, sizeof(g_hb));
    memset(g_sys, 's', sys_len);
    g_sys[sys_len] = '\0';
    g_hb[0].role = HU_ROLE_SYSTEM;
    g_hb[0].content = g_sys;
    g_hb[0].content_len = sys_len;
    for (size_t i = 1; i <= prior + 1; i++) {
        size_t len = (i == prior + 1) ? 200 : each_len;
        memset(g_body[i], 'a' + (int)(i % 26), len);
        g_body[i][len] = '\0';
        g_hb[i].role = (i % 2) ? HU_ROLE_USER : HU_ROLE_ASSISTANT;
        g_hb[i].content = g_body[i];
        g_hb[i].content_len = len;
    }
    return prior + 2;
}

static size_t hb_total(size_t n) {
    size_t t = 0;
    for (size_t i = 0; i < n; i++)
        t += hu_chat_message_estimate_bytes(&g_hb[i]);
    return t;
}

static void test_history_budget_mode_defaults_off(void) {
    unsetenv("HU_HISTORY_BUDGET");
    HU_ASSERT_EQ((int)hu_history_budget_mode(), (int)HU_GATE_OFF);
    setenv("HU_HISTORY_BUDGET", "shadow", 1);
    HU_ASSERT_EQ((int)hu_history_budget_mode(), (int)HU_GATE_SHADOW);
    setenv("HU_HISTORY_BUDGET", "live", 1);
    HU_ASSERT_EQ((int)hu_history_budget_mode(), (int)HU_GATE_LIVE);
    setenv("HU_HISTORY_BUDGET", "nope", 1);
    HU_ASSERT_EQ((int)hu_history_budget_mode(), (int)HU_GATE_OFF);
    unsetenv("HU_HISTORY_BUDGET");
}

static void test_history_budget_max_total_parse_is_clamped(void) {
    unsetenv("HU_HISTORY_BUDGET_MAX_TOTAL_BYTES");
    HU_ASSERT_EQ(hu_history_budget_max_total(), HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT);
    setenv("HU_HISTORY_BUDGET_MAX_TOTAL_BYTES", "51200", 1);
    HU_ASSERT_EQ(hu_history_budget_max_total(), 51200);
    setenv("HU_HISTORY_BUDGET_MAX_TOTAL_BYTES", "4096", 1);
    HU_ASSERT_EQ(hu_history_budget_max_total(), HU_HISTORY_BUDGET_MAX_TOTAL_FLOOR);
    setenv("HU_HISTORY_BUDGET_MAX_TOTAL_BYTES", "999999999", 1);
    HU_ASSERT_EQ(hu_history_budget_max_total(), HU_HISTORY_BUDGET_MAX_TOTAL_CEIL);
    setenv("HU_HISTORY_BUDGET_MAX_TOTAL_BYTES", "lots", 1);
    HU_ASSERT_EQ(hu_history_budget_max_total(), HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT);
    unsetenv("HU_HISTORY_BUDGET_MAX_TOTAL_BYTES");
}

/* The headline pre/post contract: a 22 KB system prompt with 4 short prior
 * messages. Legacy drops them all; LIVE keeps them all. */
static void test_history_budget_22k_system_prompt_off_drops_live_keeps(void) {
    size_t n = hb_fill(22 * 1024, 4, 1000);
    const char *oldest = g_hb[1].content;
    const char *current = g_hb[n - 1].content;
    hu_history_budget_plan_t plan;
    size_t off =
        hu_history_budget_fit(g_hb, n, HU_GATE_OFF, HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT, &plan);
    HU_ASSERT_EQ(off, 2);
    HU_ASSERT_TRUE(g_hb[1].content == current);
    HU_ASSERT_EQ(plan.msgs_before, 6);
    HU_ASSERT_EQ(plan.msgs_after_legacy, 2);
    HU_ASSERT_EQ(plan.msgs_after_scoped, 6);
    HU_ASSERT_EQ(plan.sys_bytes, 22 * 1024);

    n = hb_fill(22 * 1024, 4, 1000);
    size_t live =
        hu_history_budget_fit(g_hb, n, HU_GATE_LIVE, HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT, NULL);
    HU_ASSERT_EQ(live, 6);
    HU_ASSERT_TRUE(g_hb[1].content == oldest);
    HU_ASSERT_TRUE(g_hb[5].content == current);
}

/* SHADOW computes the comparison but sends exactly what OFF sends. */
static void test_history_budget_shadow_sends_legacy_result(void) {
    size_t n = hb_fill(22 * 1024, 4, 1000);
    hu_chat_message_t expect[HB_MAX];
    size_t off =
        hu_history_budget_fit(g_hb, n, HU_GATE_OFF, HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT, NULL);
    memcpy(expect, g_hb, off * sizeof(hu_chat_message_t));
    n = hb_fill(22 * 1024, 4, 1000);
    hu_history_budget_plan_t plan;
    size_t sh =
        hu_history_budget_fit(g_hb, n, HU_GATE_SHADOW, HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT, &plan);
    HU_ASSERT_EQ(sh, off);
    HU_ASSERT_EQ(memcmp(g_hb, expect, off * sizeof(hu_chat_message_t)), 0);
    HU_ASSERT_GT(plan.msgs_after_scoped, plan.msgs_after_legacy);
}

/* OFF reproduces the legacy A1b algorithm: system + history under 20 KB. */
static void test_history_budget_off_matches_legacy_total_budget(void) {
    size_t n = hb_fill(6 * 1024, 30, 1000);
    const char *current = g_hb[n - 1].content;
    size_t off =
        hu_history_budget_fit(g_hb, n, HU_GATE_OFF, HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT, NULL);
    HU_ASSERT_TRUE(off < n);
    HU_ASSERT_LE(hb_total(off), HU_HISTORY_BUDGET_BYTES);
    HU_ASSERT_TRUE(g_hb[off - 1].content == current);
    /* the next-older message would not have fit */
    HU_ASSERT_GT(hb_total(off) + 1000, HU_HISTORY_BUDGET_BYTES);
}

/* LIVE: history alone under 20 KB, and system + history under max_total. */
static void test_history_budget_live_caps_total(void) {
    size_t n = hb_fill(22 * 1024, 30, 1000);
    const char *current = g_hb[n - 1].content;
    const char *newest_prior = g_hb[n - 2].content;
    size_t live =
        hu_history_budget_fit(g_hb, n, HU_GATE_LIVE, HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT, NULL);
    HU_ASSERT_TRUE(live > 2 && live < n);
    HU_ASSERT_LE(hb_total(live), HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT);
    HU_ASSERT_LE(hb_total(live) - 22 * 1024, HU_HISTORY_BUDGET_BYTES);
    HU_ASSERT_TRUE(g_hb[live - 1].content == current);
    HU_ASSERT_TRUE(g_hb[live - 2].content == newest_prior);
    /* a smaller cap binds before the 20 KB history budget does */
    n = hb_fill(22 * 1024, 30, 1000);
    size_t tight = hu_history_budget_fit(g_hb, n, HU_GATE_LIVE, 30 * 1024, NULL);
    HU_ASSERT_TRUE(tight < live);
    HU_ASSERT_LE(hb_total(tight), 30 * 1024);
}

/* A system prompt over the cap leaves room for nothing but the current. */
static void test_history_budget_live_system_over_cap_keeps_current_only(void) {
    size_t n = hb_fill(50 * 1024, 4, 1000);
    const char *current = g_hb[n - 1].content;
    size_t live =
        hu_history_budget_fit(g_hb, n, HU_GATE_LIVE, HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT, NULL);
    HU_ASSERT_EQ(live, 2);
    HU_ASSERT_TRUE(g_hb[1].content == current);
}

static void test_history_budget_planners_never_drop_current(void) {
    size_t n = hb_fill(30 * 1024, 3, 2000);
    HU_ASSERT_EQ(hu_history_budget_keep_from_legacy(g_hb, n, HU_HISTORY_BUDGET_BYTES), n - 1);
    HU_ASSERT_EQ(hu_history_budget_keep_from_scoped(g_hb, n, 0, 0), n - 1);
    HU_ASSERT_EQ(hu_history_budget_keep_from_scoped(g_hb, n, HU_HISTORY_BUDGET_BYTES,
                                                    HU_HISTORY_BUDGET_MAX_TOTAL_CEIL),
                 1);
}

/* Tool iterations: fit_history runs with the tool results last. Over budget,
 * the old planner kept only the trailing tool result, which orphaned it from
 * its assistant tool_calls message and dropped the user's question. The
 * planner must keep the last USER message and everything after it. */
static hu_tool_call_t g_tc[1];

static size_t hb_fill_tool_turn(size_t sys_len, size_t tool_len) {
    memset(g_hb, 0, sizeof(g_hb));
    memset(g_sys, 's', sys_len);
    g_sys[sys_len] = '\0';
    g_hb[0].role = HU_ROLE_SYSTEM;
    g_hb[0].content = g_sys;
    g_hb[0].content_len = sys_len;
    g_hb[1].role = HU_ROLE_USER;
    g_hb[1].content = "what's the weather";
    g_hb[1].content_len = 18;
    memset(g_tc, 0, sizeof(g_tc));
    g_hb[2].role = HU_ROLE_ASSISTANT;
    g_hb[2].tool_calls = g_tc;
    g_hb[2].tool_calls_count = 1;
    for (size_t i = 3; i <= 4; i++) {
        memset(g_body[i], 't', tool_len);
        g_body[i][tool_len] = '\0';
        g_hb[i].role = HU_ROLE_TOOL;
        g_hb[i].content = g_body[i];
        g_hb[i].content_len = tool_len;
    }
    return 5;
}

static void test_history_budget_tool_turn_keeps_user_and_tool_calls(void) {
    hu_gate_mode_t modes[] = {HU_GATE_OFF, HU_GATE_SHADOW, HU_GATE_LIVE};
    for (size_t m = 0; m < 3; m++) {
        size_t n = hb_fill_tool_turn(22 * 1024, 1500); /* over both budgets */
        size_t out =
            hu_history_budget_fit(g_hb, n, modes[m], HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT, NULL);
        HU_ASSERT_EQ(out, 5);
        HU_ASSERT_TRUE(g_hb[1].role == HU_ROLE_USER);
        HU_ASSERT_TRUE(g_hb[2].role == HU_ROLE_ASSISTANT);
        HU_ASSERT_EQ(g_hb[2].tool_calls_count, 1);
        HU_ASSERT_TRUE(g_hb[3].role == HU_ROLE_TOOL);
    }
}

/* Older turns still drop, and the cut never lands between a tool_calls
 * message and its results. [sys, u, a(tc), tool, a, u, a(tc), tool, tool]. */
static void test_history_budget_cut_never_orphans_tool_result(void) {
    memset(g_hb, 0, sizeof(g_hb));
    memset(g_sys, 's', 21 * 1024);
    g_hb[0].role = HU_ROLE_SYSTEM;
    g_hb[0].content = g_sys;
    g_hb[0].content_len = 21 * 1024;
    hu_role_t roles[] = {HU_ROLE_USER, HU_ROLE_ASSISTANT, HU_ROLE_TOOL, HU_ROLE_ASSISTANT,
                         HU_ROLE_USER, HU_ROLE_ASSISTANT, HU_ROLE_TOOL, HU_ROLE_TOOL};
    for (size_t i = 0; i < 8; i++) {
        memset(g_body[i + 1], 'x', 1000);
        g_body[i + 1][1000] = '\0';
        g_hb[i + 1].role = roles[i];
        g_hb[i + 1].content = g_body[i + 1];
        g_hb[i + 1].content_len = 1000;
        if (i == 1 || i == 5) {
            g_hb[i + 1].tool_calls = g_tc;
            g_hb[i + 1].tool_calls_count = 1;
        }
    }
    size_t out =
        hu_history_budget_fit(g_hb, 9, HU_GATE_OFF, HU_HISTORY_BUDGET_MAX_TOTAL_DEFAULT, NULL);
    HU_ASSERT_TRUE(out < 9);
    HU_ASSERT_TRUE(g_hb[1].role != HU_ROLE_TOOL);
    for (size_t i = 1; i < out; i++) {
        if (g_hb[i].role == HU_ROLE_TOOL)
            HU_ASSERT_TRUE(g_hb[i - 1].role == HU_ROLE_TOOL || g_hb[i - 1].tool_calls_count > 0);
    }
    bool user = false;
    for (size_t i = 1; i < out; i++)
        user = user || g_hb[i].role == HU_ROLE_USER;
    HU_ASSERT_TRUE(user);
}

/* Wiring: the agent's real entry point honours the gate. */
static void test_agent_fit_history_honours_history_budget_gate(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    unsetenv("HU_HISTORY_BUDGET");
    unsetenv("HU_HISTORY_BUDGET_MAX_TOTAL_BYTES");
    size_t n = hb_fill(22 * 1024, 4, 1000);
    HU_ASSERT_EQ(hu_agent_internal_fit_history(&agent, g_hb, n), 2);
    setenv("HU_HISTORY_BUDGET", "live", 1);
    n = hb_fill(22 * 1024, 4, 1000);
    HU_ASSERT_EQ(hu_agent_internal_fit_history(&agent, g_hb, n), 6);
    unsetenv("HU_HISTORY_BUDGET");
}

void run_history_budget_tests(void) {
    HU_TEST_SUITE("history_budget");
    HU_RUN_TEST(test_history_budget_mode_defaults_off);
    HU_RUN_TEST(test_history_budget_max_total_parse_is_clamped);
    HU_RUN_TEST(test_history_budget_22k_system_prompt_off_drops_live_keeps);
    HU_RUN_TEST(test_history_budget_shadow_sends_legacy_result);
    HU_RUN_TEST(test_history_budget_off_matches_legacy_total_budget);
    HU_RUN_TEST(test_history_budget_live_caps_total);
    HU_RUN_TEST(test_history_budget_live_system_over_cap_keeps_current_only);
    HU_RUN_TEST(test_history_budget_planners_never_drop_current);
    HU_RUN_TEST(test_agent_fit_history_honours_history_budget_gate);
    HU_RUN_TEST(test_history_budget_tool_turn_keeps_user_and_tool_calls);
    HU_RUN_TEST(test_history_budget_cut_never_orphans_tool_result);
}
