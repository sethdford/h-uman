#include "seaclaw/agent/team.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <string.h>

#define SC_TEAM_INIT_CAP 8

/* ── Config parsing ─────────────────────────────────────────────────────── */

static sc_autonomy_level_t autonomy_from_string(const char *s) {
    if (!s)
        return SC_AUTONOMY_ASSISTED;
    if (strcmp(s, "locked") == 0 || strcmp(s, "read_only") == 0 ||
        strcmp(s, "readonly") == 0)
        return SC_AUTONOMY_LOCKED;
    if (strcmp(s, "supervised") == 0)
        return SC_AUTONOMY_SUPERVISED;
    if (strcmp(s, "assisted") == 0)
        return SC_AUTONOMY_ASSISTED;
    if (strcmp(s, "autonomous") == 0 || strcmp(s, "full") == 0)
        return SC_AUTONOMY_AUTONOMOUS;
    return SC_AUTONOMY_ASSISTED;
}

static sc_error_t parse_tools_array(sc_allocator_t *a, char ***out, size_t *out_len,
                                    const sc_json_value_t *arr) {
    if (!arr || arr->type != SC_JSON_ARRAY)
        return SC_OK;
    size_t n = 0;
    for (size_t i = 0; i < arr->data.array.len; i++) {
        if (arr->data.array.items[i] && arr->data.array.items[i]->type == SC_JSON_STRING)
            n++;
    }
    if (n == 0) {
        *out = NULL;
        *out_len = 0;
        return SC_OK;
    }
    char **list = (char **)a->alloc(a->ctx, n * sizeof(char *));
    if (!list)
        return SC_ERR_OUT_OF_MEMORY;
    size_t j = 0;
    for (size_t i = 0; i < arr->data.array.len && j < n; i++) {
        const sc_json_value_t *v = arr->data.array.items[i];
        if (!v || v->type != SC_JSON_STRING)
            continue;
        const char *s = v->data.string.ptr;
        if (s)
            list[j++] = sc_strdup(a, s);
    }
    *out = list;
    *out_len = j;
    return SC_OK;
}

static void free_config_member(sc_allocator_t *a, sc_team_config_member_t *m) {
    if (!a || !m)
        return;
    if (m->name) {
        a->free(a->ctx, m->name, strlen(m->name) + 1);
        m->name = NULL;
    }
    if (m->role) {
        a->free(a->ctx, m->role, strlen(m->role) + 1);
        m->role = NULL;
    }
    if (m->allowed_tools) {
        for (size_t i = 0; i < m->allowed_tools_count; i++) {
            if (m->allowed_tools[i])
                a->free(a->ctx, m->allowed_tools[i], strlen(m->allowed_tools[i]) + 1);
        }
        a->free(a->ctx, m->allowed_tools, m->allowed_tools_count * sizeof(char *));
        m->allowed_tools = NULL;
        m->allowed_tools_count = 0;
    }
    if (m->model) {
        a->free(a->ctx, m->model, strlen(m->model) + 1);
        m->model = NULL;
    }
}

sc_error_t sc_team_config_parse(sc_allocator_t *alloc, const char *json, size_t json_len,
    sc_team_config_t *out) {
    if (!alloc || !json || !out)
        return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, json, json_len, &root);
    if (err != SC_OK || !root || root->type != SC_JSON_OBJECT) {
        if (root)
            sc_json_free(alloc, root);
        return err != SC_OK ? err : SC_ERR_PARSE;
    }

    const char *name = sc_json_get_string(root, "name");
    if (name)
        out->name = sc_strdup(alloc, name);

    const char *base_branch = sc_json_get_string(root, "base_branch");
    if (base_branch)
        out->base_branch = sc_strdup(alloc, base_branch);

    sc_json_value_t *members_arr = sc_json_object_get(root, "members");
    if (!members_arr || members_arr->type != SC_JSON_ARRAY) {
        sc_json_free(alloc, root);
        return SC_OK;
    }

    size_t n = members_arr->data.array.len;
    if (n == 0) {
        sc_json_free(alloc, root);
        return SC_OK;
    }

    sc_team_config_member_t *members =
        (sc_team_config_member_t *)alloc->alloc(alloc->ctx, n * sizeof(sc_team_config_member_t));
    if (!members) {
        sc_json_free(alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(members, 0, n * sizeof(sc_team_config_member_t));
    out->members = members;
    out->members_count = 0;

    for (size_t i = 0; i < n; i++) {
        sc_json_value_t *item = members_arr->data.array.items[i];
        if (!item || item->type != SC_JSON_OBJECT)
            continue;

        sc_team_config_member_t *m = &members[out->members_count];
        const char *mname = sc_json_get_string(item, "name");
        if (mname)
            m->name = sc_strdup(alloc, mname);

        const char *role = sc_json_get_string(item, "role");
        if (role)
            m->role = sc_strdup(alloc, role);

        const char *autonomy_str = sc_json_get_string(item, "autonomy");
        m->autonomy = autonomy_from_string(autonomy_str);

        sc_json_value_t *tools = sc_json_object_get(item, "tools");
        if (tools && tools->type == SC_JSON_ARRAY)
            parse_tools_array(alloc, &m->allowed_tools, &m->allowed_tools_count, tools);

        const char *model = sc_json_get_string(item, "model");
        if (model)
            m->model = sc_strdup(alloc, model);

        m->active = true;
        out->members_count++;
    }

    sc_json_free(alloc, root);
    return SC_OK;
}

void sc_team_config_free(sc_allocator_t *alloc, sc_team_config_t *cfg) {
    if (!alloc || !cfg)
        return;
    if (cfg->name) {
        alloc->free(alloc->ctx, cfg->name, strlen(cfg->name) + 1);
        cfg->name = NULL;
    }
    if (cfg->base_branch) {
        alloc->free(alloc->ctx, cfg->base_branch, strlen(cfg->base_branch) + 1);
        cfg->base_branch = NULL;
    }
    if (cfg->members) {
        for (size_t i = 0; i < cfg->members_count; i++)
            free_config_member(alloc, &cfg->members[i]);
        alloc->free(alloc->ctx, cfg->members, cfg->members_count * sizeof(sc_team_config_member_t));
        cfg->members = NULL;
        cfg->members_count = 0;
    }
}

const sc_team_config_member_t *sc_team_config_get_member(const sc_team_config_t *cfg,
    const char *name) {
    if (!cfg || !name)
        return NULL;
    for (size_t i = 0; i < cfg->members_count; i++) {
        if (cfg->members[i].name && strcmp(cfg->members[i].name, name) == 0)
            return &cfg->members[i];
    }
    return NULL;
}

const sc_team_config_member_t *sc_team_config_get_by_role(const sc_team_config_t *cfg,
    const char *role) {
    if (!cfg || !role)
        return NULL;
    for (size_t i = 0; i < cfg->members_count; i++) {
        if (cfg->members[i].role && strcmp(cfg->members[i].role, role) == 0)
            return &cfg->members[i];
    }
    return NULL;
}

/* ── Runtime team (sc_team_t) ───────────────────────────────────────────── */

struct sc_team {
    sc_allocator_t *alloc;
    char *name;
    sc_team_member_t *members;
    size_t count;
    size_t capacity;
};

sc_team_role_t sc_team_role_from_string(const char *s) {
    if (!s)
        return SC_ROLE_BUILDER;
    if (strcmp(s, "lead") == 0)
        return SC_ROLE_LEAD;
    if (strcmp(s, "builder") == 0)
        return SC_ROLE_BUILDER;
    if (strcmp(s, "reviewer") == 0)
        return SC_ROLE_REVIEWER;
    if (strcmp(s, "tester") == 0)
        return SC_ROLE_TESTER;
    return SC_ROLE_BUILDER;
}

static sc_error_t grow_if_needed(sc_team_t *team) {
    if (team->count < team->capacity)
        return SC_OK;
    size_t new_cap = team->capacity == 0 ? SC_TEAM_INIT_CAP : team->capacity * 2;
    sc_team_member_t *n = (sc_team_member_t *)team->alloc->alloc(team->alloc->ctx,
        new_cap * sizeof(sc_team_member_t));
    if (!n)
        return SC_ERR_OUT_OF_MEMORY;
    memset(n, 0, new_cap * sizeof(sc_team_member_t));
    if (team->members) {
        memcpy(n, team->members, team->count * sizeof(sc_team_member_t));
        team->alloc->free(team->alloc->ctx, team->members,
            team->capacity * sizeof(sc_team_member_t));
    }
    team->members = n;
    team->capacity = new_cap;
    return SC_OK;
}

static void free_runtime_member(sc_allocator_t *a, sc_team_member_t *m) {
    if (!a || !m)
        return;
    if (m->name) {
        a->free(a->ctx, m->name, strlen(m->name) + 1);
        m->name = NULL;
    }
}

sc_team_t *sc_team_create(sc_allocator_t *alloc, const char *name) {
    if (!alloc)
        return NULL;
    sc_team_t *team = (sc_team_t *)alloc->alloc(alloc->ctx, sizeof(*team));
    if (!team)
        return NULL;
    memset(team, 0, sizeof(*team));
    team->alloc = alloc;
    if (name && name[0])
        team->name = sc_strdup(alloc, name);
    return team;
}

void sc_team_destroy(sc_team_t *team) {
    if (!team)
        return;
    sc_allocator_t *a = team->alloc;
    for (size_t i = 0; i < team->count; i++)
        free_runtime_member(a, &team->members[i]);
    if (team->name)
        a->free(a->ctx, team->name, strlen(team->name) + 1);
    if (team->members)
        a->free(a->ctx, team->members, team->capacity * sizeof(sc_team_member_t));
    a->free(a->ctx, team, sizeof(*team));
}

sc_error_t sc_team_add_member(sc_team_t *team, uint64_t agent_id, const char *name,
    sc_team_role_t role, uint8_t autonomy_level) {
    if (!team)
        return SC_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < team->count; i++) {
        if (team->members[i].agent_id == agent_id)
            return SC_ERR_ALREADY_EXISTS;
    }
    sc_error_t err = grow_if_needed(team);
    if (err != SC_OK)
        return err;

    sc_team_member_t *m = &team->members[team->count++];
    m->agent_id = agent_id;
    m->name = name && name[0] ? sc_strdup(team->alloc, name) : NULL;
    m->role = role;
    m->autonomy_level = autonomy_level;
    m->active = true;
    return SC_OK;
}

sc_error_t sc_team_remove_member(sc_team_t *team, uint64_t agent_id) {
    if (!team)
        return SC_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < team->count; i++) {
        if (team->members[i].agent_id == agent_id) {
            free_runtime_member(team->alloc, &team->members[i]);
            memmove(&team->members[i], &team->members[i + 1],
                (team->count - 1 - i) * sizeof(sc_team_member_t));
            team->count--;
            return SC_OK;
        }
    }
    return SC_ERR_NOT_FOUND;
}

const sc_team_member_t *sc_team_get_member(sc_team_t *team, uint64_t agent_id) {
    if (!team)
        return NULL;
    for (size_t i = 0; i < team->count; i++) {
        if (team->members[i].agent_id == agent_id && team->members[i].active)
            return &team->members[i];
    }
    return NULL;
}

sc_error_t sc_team_list_members(sc_team_t *team, sc_team_member_t **out,
    size_t *count) {
    if (!team || !out || !count)
        return SC_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *count = 0;
    if (team->count == 0)
        return SC_OK;

    sc_team_member_t *arr = (sc_team_member_t *)team->alloc->alloc(team->alloc->ctx,
        team->count * sizeof(sc_team_member_t));
    if (!arr)
        return SC_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < team->count; i++) {
        arr[i].agent_id = team->members[i].agent_id;
        arr[i].name = team->members[i].name ? sc_strdup(team->alloc, team->members[i].name) : NULL;
        arr[i].role = team->members[i].role;
        arr[i].autonomy_level = team->members[i].autonomy_level;
        arr[i].active = team->members[i].active;
    }
    *out = arr;
    *count = team->count;
    return SC_OK;
}

const char *sc_team_name(const sc_team_t *team) {
    return team ? team->name : NULL;
}

size_t sc_team_member_count(const sc_team_t *team) {
    return team ? team->count : 0;
}

static bool tool_matches(const char *tool_name, const char *pattern) {
    if (!tool_name || !pattern)
        return false;
    return strcmp(tool_name, pattern) == 0;
}

bool sc_team_role_allows_tool(sc_team_role_t role, const char *tool_name) {
    if (!tool_name)
        return false;
    switch (role) {
    case SC_ROLE_LEAD:
        return true;
    case SC_ROLE_BUILDER:
        return !tool_matches(tool_name, "agent_spawn");
    case SC_ROLE_REVIEWER:
        return tool_matches(tool_name, "file_read") ||
               tool_matches(tool_name, "shell") ||
               tool_matches(tool_name, "memory_recall");
    case SC_ROLE_TESTER:
        return tool_matches(tool_name, "shell") ||
               tool_matches(tool_name, "file_read") ||
               tool_matches(tool_name, "file_write");
    }
    return false;
}
