#ifndef HU_PERMISSION_H
#define HU_PERMISSION_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

/* Forward declaration — avoids circular include with agent.h */
struct hu_agent;

/* ──────────────────────────────────────────────────────────────────────────
 * Permission tiers: ReadOnly < WorkspaceWrite < DangerFullAccess < Deny
 *
 * Each tool is classified into exactly one tier. An agent has a current
 * permission level; tool execution is allowed only when
 *   agent->permission_level >= tool_required_level.
 *
 * HU_PERM_DENY is a sentinel above the highest agent-assignable tier. No
 * agent is ever constructed at DENY, so any tool whose required level is
 * DENY is unreachable. Unknown tools (e.g. dynamically loaded MCP tools)
 * are classified at DENY so they cannot run until explicitly registered.
 *
 * Temporary escalation allows a single tool call at a higher tier,
 * resetting automatically after execution. Escalation to DENY is rejected.
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_permission_level {
    HU_PERM_READ_ONLY = 0,          /* search, read, list, recall */
    HU_PERM_WORKSPACE_WRITE = 1,    /* file_write, shell, git, browser */
    HU_PERM_DANGER_FULL_ACCESS = 2, /* agent_spawn, delegate, cron, services */
    HU_PERM_DENY = 3,               /* sentinel: unknown/unregistered tools */
} hu_permission_level_t;

/**
 * Check whether `current` permission level is sufficient for `required`.
 * Returns true if current >= required.
 */
bool hu_permission_check(hu_permission_level_t current, hu_permission_level_t required);

/**
 * Look up the required permission level for a tool by name.
 *
 * Unknown or NULL tool names return HU_PERM_DENY — a sentinel above every
 * agent-assignable tier — so unregistered tools (including dynamically
 * loaded MCP tools) cannot satisfy hu_permission_check() for any caller.
 */
hu_permission_level_t hu_permission_get_tool_level(const char *tool_name);

/**
 * Temporarily escalate an agent's permission level for a single tool call.
 * The escalated level is stored on the agent and must be reset after the
 * tool executes via hu_permission_reset_escalation().
 *
 * Returns HU_ERR_INVALID_ARGUMENT if new_level <= agent's current level,
 * or if agent/tool_name is NULL.
 */
hu_error_t hu_permission_escalate_temporary(struct hu_agent *agent, hu_permission_level_t new_level,
                                            const char *tool_name);

/**
 * Reset any temporary escalation on the agent, restoring the base level.
 */
void hu_permission_reset_escalation(struct hu_agent *agent);

/**
 * Return a human-readable name for a permission level.
 * Never returns NULL.
 */
const char *hu_permission_level_name(hu_permission_level_t level);

/**
 * Count duplicate tool_name entries in the static permission table. Returns 0
 * for a healthy table. Exposed so a regression test can pin uniqueness — the
 * lookup returns the first match, so a duplicate silently masks the later
 * classification (audit 2026-05-16 found `persona` listed twice with
 * conflicting tiers).
 */
size_t hu_permission_table_duplicate_count(void);

#endif /* HU_PERMISSION_H */
