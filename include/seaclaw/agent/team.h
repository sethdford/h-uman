#ifndef SC_TEAM_H
#define SC_TEAM_H

#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/security.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Runtime team (sc_team_t) ───────────────────────────────────────────── */

typedef enum sc_team_role {
    SC_ROLE_LEAD,       /* orchestrates, can spawn others */
    SC_ROLE_BUILDER,    /* implements features */
    SC_ROLE_REVIEWER,   /* reviews code, read-only tools */
    SC_ROLE_TESTER,     /* runs tests, can execute shell */
} sc_team_role_t;

typedef struct sc_team_member {
    uint64_t agent_id;
    char *name;           /* human-readable name */
    sc_team_role_t role;
    uint8_t autonomy_level; /* per-member autonomy override */
    bool active;
} sc_team_member_t;

typedef struct sc_team sc_team_t;

sc_team_t *sc_team_create(sc_allocator_t *alloc, const char *name);
void sc_team_destroy(sc_team_t *team);

sc_error_t sc_team_add_member(sc_team_t *team, uint64_t agent_id, const char *name,
    sc_team_role_t role, uint8_t autonomy_level);
sc_error_t sc_team_remove_member(sc_team_t *team, uint64_t agent_id);

const sc_team_member_t *sc_team_get_member(sc_team_t *team, uint64_t agent_id);
sc_error_t sc_team_list_members(sc_team_t *team, sc_team_member_t **out,
    size_t *count);

const char *sc_team_name(const sc_team_t *team);
size_t sc_team_member_count(const sc_team_t *team);

/* Role-based tool filtering: which tools does this role allow? */
bool sc_team_role_allows_tool(sc_team_role_t role, const char *tool_name);

/* Parse role from string: "lead", "builder", "reviewer", "tester" */
sc_team_role_t sc_team_role_from_string(const char *s);

/* ── Team config (JSON parsing) ─────────────────────────────────────────── */

typedef struct sc_team_config_member {
    char *name;
    char *role;
    sc_autonomy_level_t autonomy;
    char **allowed_tools;
    size_t allowed_tools_count;
    char *model;
    uint64_t agent_id;
    bool active;
} sc_team_config_member_t;

typedef struct sc_team_config {
    char *name;
    sc_team_config_member_t *members;
    size_t members_count;
    char *base_branch;
} sc_team_config_t;

sc_error_t sc_team_config_parse(sc_allocator_t *alloc, const char *json,
    size_t json_len, sc_team_config_t *out);
void sc_team_config_free(sc_allocator_t *alloc, sc_team_config_t *cfg);

const sc_team_config_member_t *sc_team_config_get_member(
    const sc_team_config_t *cfg, const char *name);
const sc_team_config_member_t *sc_team_config_get_by_role(
    const sc_team_config_t *cfg, const char *role);

#endif /* SC_TEAM_H */
