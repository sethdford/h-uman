#include "human/permission.h"
#include "human/agent.h"
#include <string.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Static tool-permission classification table.
 *
 * Every built-in tool is listed here. Unknown tools (including dynamically
 * loaded MCP tools) return HU_PERM_DENY — a sentinel above every
 * agent-assignable tier — so they cannot satisfy hu_permission_check()
 * for any agent, regardless of escalation, until explicitly registered.
 * ────────────────────────────────────────────────────────────────────────── */

typedef struct {
    const char *tool_name;
    hu_permission_level_t level;
} hu_tool_perm_entry_t;

static const hu_tool_perm_entry_t HU_TOOL_PERMISSIONS[] = {
    /* ── READ_ONLY: search, read, list, recall, inspect ── */
    {"web_search", HU_PERM_READ_ONLY},
    {"web_fetch", HU_PERM_READ_ONLY},
    {"file_read", HU_PERM_READ_ONLY},
    {"memory_recall", HU_PERM_READ_ONLY},
    {"memory_list", HU_PERM_READ_ONLY},
    {"image", HU_PERM_READ_ONLY},
    {"screenshot", HU_PERM_READ_ONLY},
    {"pdf", HU_PERM_READ_ONLY},
    {"diff", HU_PERM_READ_ONLY},
    {"agent_query", HU_PERM_READ_ONLY},
    {"schema", HU_PERM_READ_ONLY},
    {"report", HU_PERM_READ_ONLY},
    {"analytics", HU_PERM_READ_ONLY},
    {"hardware_info", HU_PERM_READ_ONLY},
    {"hardware_memory", HU_PERM_READ_ONLY},
    {"cron_list", HU_PERM_READ_ONLY},
    {"cron_runs", HU_PERM_READ_ONLY},
    {"doc_ingest", HU_PERM_READ_ONLY},
    {"meeting_transcribe", HU_PERM_READ_ONLY},

    /* ── WORKSPACE_WRITE: modify workspace, network I/O, messaging ── */
    {"file_write", HU_PERM_WORKSPACE_WRITE},
    {"file_edit", HU_PERM_WORKSPACE_WRITE},
    {"file_append", HU_PERM_WORKSPACE_WRITE},
    {"shell", HU_PERM_WORKSPACE_WRITE},
    {"git", HU_PERM_WORKSPACE_WRITE},
    {"http_request", HU_PERM_WORKSPACE_WRITE},
    {"browser", HU_PERM_WORKSPACE_WRITE},
    {"browser_open", HU_PERM_WORKSPACE_WRITE},
    {"browser_use", HU_PERM_WORKSPACE_WRITE},
    {"notebook", HU_PERM_WORKSPACE_WRITE},
    {"canvas", HU_PERM_WORKSPACE_WRITE},
    {"apply_patch", HU_PERM_WORKSPACE_WRITE},
    {"memory_store", HU_PERM_WORKSPACE_WRITE},
    {"memory_forget", HU_PERM_WORKSPACE_WRITE},
    {"save_for_later", HU_PERM_WORKSPACE_WRITE},
    {"bff_memory", HU_PERM_WORKSPACE_WRITE},
    {"message", HU_PERM_WORKSPACE_WRITE},
    {"send_message", HU_PERM_WORKSPACE_WRITE},
    {"pushover", HU_PERM_WORKSPACE_WRITE},
    {"image_gen", HU_PERM_WORKSPACE_WRITE},
    {"code_sandbox", HU_PERM_WORKSPACE_WRITE},
    {"spreadsheet", HU_PERM_WORKSPACE_WRITE},
    {"broadcast", HU_PERM_WORKSPACE_WRITE},
    {"calendar", HU_PERM_WORKSPACE_WRITE},
    {"skill_write", HU_PERM_WORKSPACE_WRITE},
    {"pwa", HU_PERM_WORKSPACE_WRITE},
    {"jira", HU_PERM_WORKSPACE_WRITE},
    {"social", HU_PERM_WORKSPACE_WRITE},
    {"facebook", HU_PERM_WORKSPACE_WRITE},
    {"instagram", HU_PERM_WORKSPACE_WRITE},
    {"twitter", HU_PERM_WORKSPACE_WRITE},
    {"crm", HU_PERM_WORKSPACE_WRITE},
    {"invoice", HU_PERM_WORKSPACE_WRITE},
    {"database", HU_PERM_WORKSPACE_WRITE},
    {"composio", HU_PERM_WORKSPACE_WRITE},
    {"i2c", HU_PERM_WORKSPACE_WRITE},
    {"spi", HU_PERM_WORKSPACE_WRITE},
    {"peripheral_ctrl", HU_PERM_WORKSPACE_WRITE},
    {"paperclip", HU_PERM_WORKSPACE_WRITE},
    /* "persona" is classified DANGER below — personas drive system prompts,
     * so a write here is a jailbreak surface, not workspace I/O. */

    /* ── DANGER_FULL_ACCESS: spawn agents, cron mutation, cloud services ── */
    {"agent_spawn", HU_PERM_DANGER_FULL_ACCESS},
    {"spawn", HU_PERM_DANGER_FULL_ACCESS},
    {"delegate", HU_PERM_DANGER_FULL_ACCESS},
    {"cron_add", HU_PERM_DANGER_FULL_ACCESS},
    {"cron_remove", HU_PERM_DANGER_FULL_ACCESS},
    {"cron_run", HU_PERM_DANGER_FULL_ACCESS},
    {"cron_update", HU_PERM_DANGER_FULL_ACCESS},
    {"schedule", HU_PERM_DANGER_FULL_ACCESS},
    {"gui_agent", HU_PERM_DANGER_FULL_ACCESS},
    {"computer_use", HU_PERM_DANGER_FULL_ACCESS},
    {"claude_code", HU_PERM_DANGER_FULL_ACCESS},
    {"skill_run", HU_PERM_DANGER_FULL_ACCESS},
    {"homeassistant", HU_PERM_DANGER_FULL_ACCESS},
    {"gcloud", HU_PERM_DANGER_FULL_ACCESS},
    {"firebase", HU_PERM_DANGER_FULL_ACCESS},
    {"workflow", HU_PERM_DANGER_FULL_ACCESS},

    /* ── Additional tools previously missing from the categorized sections.
     * Duplicates that existed earlier in the table have been removed; the
     * test_permission_table_no_duplicates regression test pins uniqueness.
     * `persona` is DANGER (not WORKSPACE) — it drives system prompts, so
     * write access is a jailbreak surface. */
    {"lsp", HU_PERM_READ_ONLY},
    {"persona", HU_PERM_DANGER_FULL_ACCESS},
    {"voice_clone", HU_PERM_WORKSPACE_WRITE},
};

#define HU_TOOL_PERMISSIONS_COUNT (sizeof(HU_TOOL_PERMISSIONS) / sizeof(HU_TOOL_PERMISSIONS[0]))

/* ────────────────────────────────────────────────────────────────────────── */

bool hu_permission_check(hu_permission_level_t current, hu_permission_level_t required) {
    return (int)current >= (int)required;
}

hu_permission_level_t hu_permission_get_tool_level(const char *tool_name) {
    if (!tool_name)
        return HU_PERM_DENY;

    for (size_t i = 0; i < HU_TOOL_PERMISSIONS_COUNT; i++) {
        if (strcmp(HU_TOOL_PERMISSIONS[i].tool_name, tool_name) == 0)
            return HU_TOOL_PERMISSIONS[i].level;
    }

    /* Unknown tools sit at the DENY sentinel — above every agent-assignable
       tier — so hu_permission_check() returns false for any current level. */
    return HU_PERM_DENY;
}

hu_error_t hu_permission_escalate_temporary(struct hu_agent *agent, hu_permission_level_t new_level,
                                            const char *tool_name) {
    if (!agent || !tool_name)
        return HU_ERR_INVALID_ARGUMENT;
    /* DENY is a sentinel above every agent-assignable tier — never a valid
     * escalation target. Without this guard, an agent at DANGER (2) could be
     * escalated to DENY (3), after which hu_permission_check(DENY, DENY)
     * returns true and unknown / unregistered tools become reachable —
     * exactly the masquerading hole this PR aims to close. */
    if ((int)new_level >= (int)HU_PERM_DENY)
        return HU_ERR_INVALID_ARGUMENT;
    if ((int)new_level <= (int)agent->permission_level)
        return HU_ERR_INVALID_ARGUMENT;

    agent->permission_level = new_level;
    agent->permission_escalated = true;
    return HU_OK;
}

void hu_permission_reset_escalation(struct hu_agent *agent) {
    if (!agent)
        return;
    if (agent->permission_escalated) {
        agent->permission_level = agent->permission_base_level;
        agent->permission_escalated = false;
    }
}

size_t hu_permission_table_duplicate_count(void) {
    size_t dups = 0;
    for (size_t i = 0; i < HU_TOOL_PERMISSIONS_COUNT; i++) {
        for (size_t j = i + 1; j < HU_TOOL_PERMISSIONS_COUNT; j++) {
            if (strcmp(HU_TOOL_PERMISSIONS[i].tool_name, HU_TOOL_PERMISSIONS[j].tool_name) == 0) {
                dups++;
                break; /* count each later occurrence once per source entry */
            }
        }
    }
    return dups;
}

const char *hu_permission_level_name(hu_permission_level_t level) {
    switch (level) {
    case HU_PERM_READ_ONLY:
        return "ReadOnly";
    case HU_PERM_WORKSPACE_WRITE:
        return "WorkspaceWrite";
    case HU_PERM_DANGER_FULL_ACCESS:
        return "DangerFullAccess";
    case HU_PERM_DENY:
        return "Deny";
    default:
        return "Unknown";
    }
}
