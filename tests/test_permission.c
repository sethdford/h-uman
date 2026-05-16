#include "human/agent.h"
#include "human/core/allocator.h"
#include "human/permission.h"
#include "test_framework.h"
#include <string.h>

/* ══════════════════════════════════════════════════════════════════════════
 * Permission tier unit tests
 * ══════════════════════════════════════════════════════════════════════════ */

/* ── hu_permission_check ── */

static void test_check_same_level_allows(void) {
    HU_ASSERT_TRUE(hu_permission_check(HU_PERM_READ_ONLY, HU_PERM_READ_ONLY));
    HU_ASSERT_TRUE(hu_permission_check(HU_PERM_WORKSPACE_WRITE, HU_PERM_WORKSPACE_WRITE));
    HU_ASSERT_TRUE(hu_permission_check(HU_PERM_DANGER_FULL_ACCESS, HU_PERM_DANGER_FULL_ACCESS));
}

static void test_check_higher_level_allows(void) {
    HU_ASSERT_TRUE(hu_permission_check(HU_PERM_DANGER_FULL_ACCESS, HU_PERM_READ_ONLY));
    HU_ASSERT_TRUE(hu_permission_check(HU_PERM_DANGER_FULL_ACCESS, HU_PERM_WORKSPACE_WRITE));
    HU_ASSERT_TRUE(hu_permission_check(HU_PERM_WORKSPACE_WRITE, HU_PERM_READ_ONLY));
}

static void test_check_lower_level_denies(void) {
    HU_ASSERT_FALSE(hu_permission_check(HU_PERM_READ_ONLY, HU_PERM_WORKSPACE_WRITE));
    HU_ASSERT_FALSE(hu_permission_check(HU_PERM_READ_ONLY, HU_PERM_DANGER_FULL_ACCESS));
    HU_ASSERT_FALSE(hu_permission_check(HU_PERM_WORKSPACE_WRITE, HU_PERM_DANGER_FULL_ACCESS));
}

/* ── hu_permission_get_tool_level ── */

static void test_get_tool_level_read_only(void) {
    HU_ASSERT_EQ(hu_permission_get_tool_level("web_search"), HU_PERM_READ_ONLY);
    HU_ASSERT_EQ(hu_permission_get_tool_level("file_read"), HU_PERM_READ_ONLY);
    HU_ASSERT_EQ(hu_permission_get_tool_level("memory_recall"), HU_PERM_READ_ONLY);
    HU_ASSERT_EQ(hu_permission_get_tool_level("memory_list"), HU_PERM_READ_ONLY);
    HU_ASSERT_EQ(hu_permission_get_tool_level("pdf"), HU_PERM_READ_ONLY);
    HU_ASSERT_EQ(hu_permission_get_tool_level("analytics"), HU_PERM_READ_ONLY);
}

static void test_get_tool_level_workspace_write(void) {
    HU_ASSERT_EQ(hu_permission_get_tool_level("file_write"), HU_PERM_WORKSPACE_WRITE);
    HU_ASSERT_EQ(hu_permission_get_tool_level("shell"), HU_PERM_WORKSPACE_WRITE);
    HU_ASSERT_EQ(hu_permission_get_tool_level("git"), HU_PERM_WORKSPACE_WRITE);
    HU_ASSERT_EQ(hu_permission_get_tool_level("browser"), HU_PERM_WORKSPACE_WRITE);
    HU_ASSERT_EQ(hu_permission_get_tool_level("notebook"), HU_PERM_WORKSPACE_WRITE);
}

static void test_get_tool_level_danger(void) {
    HU_ASSERT_EQ(hu_permission_get_tool_level("agent_spawn"), HU_PERM_DANGER_FULL_ACCESS);
    HU_ASSERT_EQ(hu_permission_get_tool_level("delegate"), HU_PERM_DANGER_FULL_ACCESS);
    HU_ASSERT_EQ(hu_permission_get_tool_level("cron_add"), HU_PERM_DANGER_FULL_ACCESS);
    HU_ASSERT_EQ(hu_permission_get_tool_level("schedule"), HU_PERM_DANGER_FULL_ACCESS);
    HU_ASSERT_EQ(hu_permission_get_tool_level("workflow"), HU_PERM_DANGER_FULL_ACCESS);
    HU_ASSERT_EQ(hu_permission_get_tool_level("gcloud"), HU_PERM_DANGER_FULL_ACCESS);
}

static void test_get_tool_level_unknown_returns_deny(void) {
    HU_ASSERT_EQ(hu_permission_get_tool_level("totally_unknown_tool"), HU_PERM_DENY);
    HU_ASSERT_EQ(hu_permission_get_tool_level(""), HU_PERM_DENY);
}

static void test_get_tool_level_null_returns_deny(void) {
    HU_ASSERT_EQ(hu_permission_get_tool_level(NULL), HU_PERM_DENY);
}

/* DENY tier sits strictly above every agent-assignable tier — so even an
   agent at DANGER_FULL_ACCESS cannot satisfy a DENY required level. This
   is the contract that keeps unknown / unregistered MCP tools unreachable. */
static void test_unknown_tool_blocked_from_danger_agent(void) {
    hu_permission_level_t required = hu_permission_get_tool_level("evil_mcp_tool");
    HU_ASSERT_EQ(required, HU_PERM_DENY);
    HU_ASSERT_FALSE(hu_permission_check(HU_PERM_DANGER_FULL_ACCESS, required));
    HU_ASSERT_FALSE(hu_permission_check(HU_PERM_WORKSPACE_WRITE, required));
    HU_ASSERT_FALSE(hu_permission_check(HU_PERM_READ_ONLY, required));
}

static void test_deny_strictly_above_danger(void) {
    HU_ASSERT_TRUE((int)HU_PERM_DENY > (int)HU_PERM_DANGER_FULL_ACCESS);
}

/* ── hu_permission_level_name ── */

static void test_level_name(void) {
    HU_ASSERT_STR_EQ(hu_permission_level_name(HU_PERM_READ_ONLY), "ReadOnly");
    HU_ASSERT_STR_EQ(hu_permission_level_name(HU_PERM_WORKSPACE_WRITE), "WorkspaceWrite");
    HU_ASSERT_STR_EQ(hu_permission_level_name(HU_PERM_DANGER_FULL_ACCESS), "DangerFullAccess");
    HU_ASSERT_STR_EQ(hu_permission_level_name(HU_PERM_DENY), "Deny");
    HU_ASSERT_STR_EQ(hu_permission_level_name((hu_permission_level_t)99), "Unknown");
}

/* ── Escalation / reset ── */

static void test_escalate_and_reset(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.permission_level = HU_PERM_READ_ONLY;
    agent.permission_base_level = HU_PERM_READ_ONLY;
    agent.permission_escalated = false;

    /* Escalate to WORKSPACE_WRITE */
    hu_error_t err = hu_permission_escalate_temporary(&agent, HU_PERM_WORKSPACE_WRITE, "shell");
    HU_ASSERT_EQ(err, HU_OK);
    HU_ASSERT_EQ(agent.permission_level, HU_PERM_WORKSPACE_WRITE);
    HU_ASSERT_TRUE(agent.permission_escalated);

    /* Check passes at escalated level */
    HU_ASSERT_TRUE(hu_permission_check(agent.permission_level, HU_PERM_WORKSPACE_WRITE));

    /* Reset restores base level */
    hu_permission_reset_escalation(&agent);
    HU_ASSERT_EQ(agent.permission_level, HU_PERM_READ_ONLY);
    HU_ASSERT_FALSE(agent.permission_escalated);
}

static void test_escalate_rejects_lower_level(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.permission_level = HU_PERM_WORKSPACE_WRITE;
    agent.permission_base_level = HU_PERM_WORKSPACE_WRITE;

    hu_error_t err = hu_permission_escalate_temporary(&agent, HU_PERM_READ_ONLY, "file_read");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    /* Level unchanged */
    HU_ASSERT_EQ(agent.permission_level, HU_PERM_WORKSPACE_WRITE);
}

static void test_escalate_rejects_same_level(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.permission_level = HU_PERM_WORKSPACE_WRITE;
    agent.permission_base_level = HU_PERM_WORKSPACE_WRITE;

    hu_error_t err = hu_permission_escalate_temporary(&agent, HU_PERM_WORKSPACE_WRITE, "shell");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

/* Regression for cursor-bot review on PR #90: an agent at DANGER (2) could be
 * escalated to DENY (3) because 3 > 2 passed the existing `<=` guard. After
 * the escalation, hu_permission_check(DENY, DENY) returned true and unknown
 * MCP tools became reachable — the very hole the DENY sentinel was added to
 * close. Guard now rejects new_level >= HU_PERM_DENY outright. */
static void test_escalate_rejects_deny_sentinel_from_danger(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.permission_level = HU_PERM_DANGER_FULL_ACCESS;
    agent.permission_base_level = HU_PERM_DANGER_FULL_ACCESS;

    hu_error_t err = hu_permission_escalate_temporary(&agent, HU_PERM_DENY, "mcp__evil__tool");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(agent.permission_level, HU_PERM_DANGER_FULL_ACCESS);
    HU_ASSERT_FALSE(agent.permission_escalated);
    /* The post-escalation contract still holds: an unknown tool stays blocked. */
    HU_ASSERT_FALSE(hu_permission_check(agent.permission_level,
                                        hu_permission_get_tool_level("mcp__evil__tool")));
}

static void test_escalate_rejects_deny_sentinel_from_read_only(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.permission_level = HU_PERM_READ_ONLY;
    agent.permission_base_level = HU_PERM_READ_ONLY;

    hu_error_t err = hu_permission_escalate_temporary(&agent, HU_PERM_DENY, "anything");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
    HU_ASSERT_EQ(agent.permission_level, HU_PERM_READ_ONLY);
}

/* Regression for Copilot review on PR #90: the table contained `persona`
 * twice with conflicting tiers. The lookup returns the first match, so the
 * later (more restrictive) classification was silently masked. Pin
 * uniqueness so this can't recur. */
static void test_permission_table_no_duplicates(void) {
    HU_ASSERT_EQ(hu_permission_table_duplicate_count(), 0);
}

/* The persona tool drives system prompts — modifying it is a jailbreak
 * surface, not workspace I/O. Pin the authoritative tier. */
static void test_persona_tool_requires_danger_tier(void) {
    HU_ASSERT_EQ(hu_permission_get_tool_level("persona"), HU_PERM_DANGER_FULL_ACCESS);
}

static void test_escalate_null_agent(void) {
    hu_error_t err = hu_permission_escalate_temporary(NULL, HU_PERM_DANGER_FULL_ACCESS, "spawn");
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_escalate_null_tool(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.permission_level = HU_PERM_READ_ONLY;
    agent.permission_base_level = HU_PERM_READ_ONLY;

    hu_error_t err = hu_permission_escalate_temporary(&agent, HU_PERM_DANGER_FULL_ACCESS, NULL);
    HU_ASSERT_EQ(err, HU_ERR_INVALID_ARGUMENT);
}

static void test_reset_no_escalation_is_noop(void) {
    hu_agent_t agent;
    memset(&agent, 0, sizeof(agent));
    agent.permission_level = HU_PERM_WORKSPACE_WRITE;
    agent.permission_base_level = HU_PERM_WORKSPACE_WRITE;
    agent.permission_escalated = false;

    hu_permission_reset_escalation(&agent);
    HU_ASSERT_EQ(agent.permission_level, HU_PERM_WORKSPACE_WRITE);
}

static void test_reset_null_agent_is_safe(void) {
    /* Must not crash */
    hu_permission_reset_escalation(NULL);
}

/* ── Integration: check against tool lookup ── */

static void test_read_only_agent_blocked_from_write_tools(void) {
    hu_permission_level_t agent_level = HU_PERM_READ_ONLY;
    hu_permission_level_t tool_level = hu_permission_get_tool_level("file_write");
    HU_ASSERT_FALSE(hu_permission_check(agent_level, tool_level));

    tool_level = hu_permission_get_tool_level("agent_spawn");
    HU_ASSERT_FALSE(hu_permission_check(agent_level, tool_level));
}

static void test_workspace_agent_allowed_write_blocked_danger(void) {
    hu_permission_level_t agent_level = HU_PERM_WORKSPACE_WRITE;

    HU_ASSERT_TRUE(hu_permission_check(agent_level, hu_permission_get_tool_level("file_read")));
    HU_ASSERT_TRUE(hu_permission_check(agent_level, hu_permission_get_tool_level("file_write")));
    HU_ASSERT_TRUE(hu_permission_check(agent_level, hu_permission_get_tool_level("shell")));
    HU_ASSERT_FALSE(hu_permission_check(agent_level, hu_permission_get_tool_level("agent_spawn")));
    HU_ASSERT_FALSE(hu_permission_check(agent_level, hu_permission_get_tool_level("delegate")));
}

/* ══════════════════════════════════════════════════════════════════════════ */

void hu_test_permission(void) {
    HU_TEST_SUITE("permission");

    HU_RUN_TEST(test_check_same_level_allows);
    HU_RUN_TEST(test_check_higher_level_allows);
    HU_RUN_TEST(test_check_lower_level_denies);
    HU_RUN_TEST(test_get_tool_level_read_only);
    HU_RUN_TEST(test_get_tool_level_workspace_write);
    HU_RUN_TEST(test_get_tool_level_danger);
    HU_RUN_TEST(test_get_tool_level_unknown_returns_deny);
    HU_RUN_TEST(test_get_tool_level_null_returns_deny);
    HU_RUN_TEST(test_unknown_tool_blocked_from_danger_agent);
    HU_RUN_TEST(test_deny_strictly_above_danger);
    HU_RUN_TEST(test_level_name);
    HU_RUN_TEST(test_escalate_and_reset);
    HU_RUN_TEST(test_escalate_rejects_lower_level);
    HU_RUN_TEST(test_escalate_rejects_same_level);
    HU_RUN_TEST(test_escalate_rejects_deny_sentinel_from_danger);
    HU_RUN_TEST(test_escalate_rejects_deny_sentinel_from_read_only);
    HU_RUN_TEST(test_permission_table_no_duplicates);
    HU_RUN_TEST(test_persona_tool_requires_danger_tier);
    HU_RUN_TEST(test_escalate_null_agent);
    HU_RUN_TEST(test_escalate_null_tool);
    HU_RUN_TEST(test_reset_no_escalation_is_noop);
    HU_RUN_TEST(test_reset_null_agent_is_safe);
    HU_RUN_TEST(test_read_only_agent_blocked_from_write_tools);
    HU_RUN_TEST(test_workspace_agent_allowed_write_blocked_danger);
}
