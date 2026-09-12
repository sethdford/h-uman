#include "human/agent/team.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <string.h>

#define HU_TEAM_INIT_CAP 8

/* ── Runtime team (hu_team_t) ───────────────────────────────────────────── */

struct hu_team {
    hu_allocator_t *alloc;
    char *name;
    hu_team_member_t *members;
    size_t count;
    size_t capacity;
};
static void free_runtime_member(hu_allocator_t *a, hu_team_member_t *m) {
    if (!a || !m)
        return;
    if (m->name) {
        a->free(a->ctx, m->name, strlen(m->name) + 1);
        m->name = NULL;
    }
}
void hu_team_destroy(hu_team_t *team) {
    if (!team)
        return;
    hu_allocator_t *a = team->alloc;
    for (size_t i = 0; i < team->count; i++)
        free_runtime_member(a, &team->members[i]);
    if (team->name)
        a->free(a->ctx, team->name, strlen(team->name) + 1);
    if (team->members)
        a->free(a->ctx, team->members, team->capacity * sizeof(hu_team_member_t));
    a->free(a->ctx, team, sizeof(*team));
}
const hu_team_member_t *hu_team_get_member(hu_team_t *team, uint64_t agent_id) {
    if (!team)
        return NULL;
    for (size_t i = 0; i < team->count; i++) {
        if (team->members[i].agent_id == agent_id && team->members[i].active)
            return &team->members[i];
    }
    return NULL;
}
static bool tool_matches(const char *tool_name, const char *pattern) {
    if (!tool_name || !pattern)
        return false;
    return strcmp(tool_name, pattern) == 0;
}

bool hu_team_role_allows_tool(hu_team_role_t role, const char *tool_name) {
    if (!tool_name)
        return false;
    switch (role) {
    case HU_ROLE_LEAD:
        return true;
    case HU_ROLE_BUILDER:
        return !tool_matches(tool_name, "agent_spawn");
    case HU_ROLE_REVIEWER:
        return tool_matches(tool_name, "file_read") || tool_matches(tool_name, "shell") ||
               tool_matches(tool_name, "memory_recall");
    case HU_ROLE_TESTER:
        return tool_matches(tool_name, "shell") || tool_matches(tool_name, "file_read") ||
               tool_matches(tool_name, "file_write");
    }
    return false;
}
