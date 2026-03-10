#ifndef HU_TEAM_H
#define HU_TEAM_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/security.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Runtime team (hu_team_t) ───────────────────────────────────────────── */

typedef enum hu_team_role {
    HU_ROLE_LEAD,     /* orchestrates, can spawn others */
    HU_ROLE_BUILDER,  /* implements features */
    HU_ROLE_REVIEWER, /* reviews code, read-only tools */
    HU_ROLE_TESTER,   /* runs tests, can execute shell */
} hu_team_role_t;

typedef struct hu_team_member {
    uint64_t agent_id;
    char *name; /* human-readable name */
    hu_team_role_t role;
    uint8_t autonomy_level; /* per-member autonomy override */
    bool active;
} hu_team_member_t;

typedef struct hu_team hu_team_t;

hu_team_t *hu_team_create(hu_allocator_t *alloc, const char *name);
void hu_team_destroy(hu_team_t *team);

hu_error_t hu_team_add_member(hu_team_t *team, uint64_t agent_id, const char *name,
                              hu_team_role_t role, uint8_t autonomy_level);
hu_error_t hu_team_remove_member(hu_team_t *team, uint64_t agent_id);

const hu_team_member_t *hu_team_get_member(hu_team_t *team, uint64_t agent_id);
hu_error_t hu_team_list_members(hu_team_t *team, hu_team_member_t **out, size_t *count);

const char *hu_team_name(const hu_team_t *team);
size_t hu_team_member_count(const hu_team_t *team);

/* Role-based tool filtering: which tools does this role allow? */
bool hu_team_role_allows_tool(hu_team_role_t role, const char *tool_name);

/* Parse role from string: "lead", "builder", "reviewer", "tester" */
hu_team_role_t hu_team_role_from_string(const char *s);

/* ── Team config (JSON parsing) ─────────────────────────────────────────── */

typedef struct hu_team_config_member {
    char *name;
    char *role;
    hu_autonomy_level_t autonomy;
    char **allowed_tools;
    size_t allowed_tools_count;
    char *model;
    uint64_t agent_id;
    bool active;
} hu_team_config_member_t;

typedef struct hu_team_config {
    char *name;
    hu_team_config_member_t *members;
    size_t members_count;
    char *base_branch;
} hu_team_config_t;

hu_error_t hu_team_config_parse(hu_allocator_t *alloc, const char *json, size_t json_len,
                                hu_team_config_t *out);
void hu_team_config_free(hu_allocator_t *alloc, hu_team_config_t *cfg);

const hu_team_config_member_t *hu_team_config_get_member(const hu_team_config_t *cfg,
                                                         const char *name);
const hu_team_config_member_t *hu_team_config_get_by_role(const hu_team_config_t *cfg,
                                                          const char *role);

#endif /* HU_TEAM_H */
