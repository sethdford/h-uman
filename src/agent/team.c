#include "human/agent/team.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <string.h>

#define HU_TEAM_INIT_CAP 8

/* ── Config parsing ─────────────────────────────────────────────────────── */

static hu_autonomy_level_t autonomy_from_string(const char *s) {
    if (!s)
        return HU_AUTONOMY_ASSISTED;
    if (strcmp(s, "locked") == 0 || strcmp(s, "read_only") == 0 || strcmp(s, "readonly") == 0)
        return HU_AUTONOMY_LOCKED;
    if (strcmp(s, "supervised") == 0)
        return HU_AUTONOMY_SUPERVISED;
    if (strcmp(s, "assisted") == 0)
        return HU_AUTONOMY_ASSISTED;
    if (strcmp(s, "autonomous") == 0 || strcmp(s, "full") == 0)
        return HU_AUTONOMY_AUTONOMOUS;
    return HU_AUTONOMY_ASSISTED;
}

static hu_error_t parse_tools_array(hu_allocator_t *a, char ***out, size_t *out_len,
                                    const hu_json_value_t *arr) {
    if (!arr || arr->type != HU_JSON_ARRAY)
        return HU_OK;
    size_t n = 0;
    for (size_t i = 0; i < arr->data.array.len; i++) {
        if (arr->data.array.items[i] && arr->data.array.items[i]->type == HU_JSON_STRING)
            n++;
    }
    if (n == 0) {
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }
    char **list = (char **)a->alloc(a->ctx, n * sizeof(char *));
    if (!list)
        return HU_ERR_OUT_OF_MEMORY;
    size_t j = 0;
    for (size_t i = 0; i < arr->data.array.len && j < n; i++) {
        const hu_json_value_t *v = arr->data.array.items[i];
        if (!v || v->type != HU_JSON_STRING)
            continue;
        const char *s = v->data.string.ptr;
        if (s)
            list[j++] = hu_strdup(a, s);
    }
    *out = list;
    *out_len = j;
    return HU_OK;
}

static void free_config_member(hu_allocator_t *a, hu_team_config_member_t *m) {
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

hu_error_t hu_team_config_parse(hu_allocator_t *alloc, const char *json, size_t json_len,
                                hu_team_config_t *out) {
    if (!alloc || !json || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK || !root || root->type != HU_JSON_OBJECT) {
        if (root)
            hu_json_free(alloc, root);
        return err != HU_OK ? err : HU_ERR_PARSE;
    }

    const char *name = hu_json_get_string(root, "name");
    if (name)
        out->name = hu_strdup(alloc, name);

    const char *base_branch = hu_json_get_string(root, "base_branch");
    if (base_branch)
        out->base_branch = hu_strdup(alloc, base_branch);

    hu_json_value_t *members_arr = hu_json_object_get(root, "members");
    if (!members_arr || members_arr->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return HU_OK;
    }

    size_t n = members_arr->data.array.len;
    if (n == 0) {
        hu_json_free(alloc, root);
        return HU_OK;
    }

    hu_team_config_member_t *members =
        (hu_team_config_member_t *)alloc->alloc(alloc->ctx, n * sizeof(hu_team_config_member_t));
    if (!members) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(members, 0, n * sizeof(hu_team_config_member_t));
    out->members = members;
    out->members_count = 0;

    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *item = members_arr->data.array.items[i];
        if (!item || item->type != HU_JSON_OBJECT)
            continue;

        hu_team_config_member_t *m = &members[out->members_count];
        const char *mname = hu_json_get_string(item, "name");
        if (mname)
            m->name = hu_strdup(alloc, mname);

        const char *role = hu_json_get_string(item, "role");
        if (role)
            m->role = hu_strdup(alloc, role);

        const char *autonomy_str = hu_json_get_string(item, "autonomy");
        m->autonomy = autonomy_from_string(autonomy_str);

        hu_json_value_t *tools = hu_json_object_get(item, "tools");
        if (tools && tools->type == HU_JSON_ARRAY)
            parse_tools_array(alloc, &m->allowed_tools, &m->allowed_tools_count, tools);

        const char *model = hu_json_get_string(item, "model");
        if (model)
            m->model = hu_strdup(alloc, model);

        m->active = true;
        out->members_count++;
    }

    hu_json_free(alloc, root);
    return HU_OK;
}

void hu_team_config_free(hu_allocator_t *alloc, hu_team_config_t *cfg) {
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
        alloc->free(alloc->ctx, cfg->members, cfg->members_count * sizeof(hu_team_config_member_t));
        cfg->members = NULL;
        cfg->members_count = 0;
    }
}

const hu_team_config_member_t *hu_team_config_get_member(const hu_team_config_t *cfg,
                                                         const char *name) {
    if (!cfg || !name)
        return NULL;
    for (size_t i = 0; i < cfg->members_count; i++) {
        if (cfg->members[i].name && strcmp(cfg->members[i].name, name) == 0)
            return &cfg->members[i];
    }
    return NULL;
}

const hu_team_config_member_t *hu_team_config_get_by_role(const hu_team_config_t *cfg,
                                                          const char *role) {
    if (!cfg || !role)
        return NULL;
    for (size_t i = 0; i < cfg->members_count; i++) {
        if (cfg->members[i].role && strcmp(cfg->members[i].role, role) == 0)
            return &cfg->members[i];
    }
    return NULL;
}

/* ── Runtime team (hu_team_t) ───────────────────────────────────────────── */

struct hu_team {
    hu_allocator_t *alloc;
    char *name;
    hu_team_member_t *members;
    size_t count;
    size_t capacity;
};

hu_team_role_t hu_team_role_from_string(const char *s) {
    if (!s)
        return HU_ROLE_BUILDER;
    if (strcmp(s, "lead") == 0)
        return HU_ROLE_LEAD;
    if (strcmp(s, "builder") == 0)
        return HU_ROLE_BUILDER;
    if (strcmp(s, "reviewer") == 0)
        return HU_ROLE_REVIEWER;
    if (strcmp(s, "tester") == 0)
        return HU_ROLE_TESTER;
    return HU_ROLE_BUILDER;
}

static hu_error_t grow_if_needed(hu_team_t *team) {
    if (team->count < team->capacity)
        return HU_OK;
    size_t new_cap = team->capacity == 0 ? HU_TEAM_INIT_CAP : team->capacity * 2;
    hu_team_member_t *n = (hu_team_member_t *)team->alloc->alloc(
        team->alloc->ctx, new_cap * sizeof(hu_team_member_t));
    if (!n)
        return HU_ERR_OUT_OF_MEMORY;
    memset(n, 0, new_cap * sizeof(hu_team_member_t));
    if (team->members) {
        memcpy(n, team->members, team->count * sizeof(hu_team_member_t));
        team->alloc->free(team->alloc->ctx, team->members,
                          team->capacity * sizeof(hu_team_member_t));
    }
    team->members = n;
    team->capacity = new_cap;
    return HU_OK;
}

static void free_runtime_member(hu_allocator_t *a, hu_team_member_t *m) {
    if (!a || !m)
        return;
    if (m->name) {
        a->free(a->ctx, m->name, strlen(m->name) + 1);
        m->name = NULL;
    }
}

hu_team_t *hu_team_create(hu_allocator_t *alloc, const char *name) {
    if (!alloc)
        return NULL;
    hu_team_t *team = (hu_team_t *)alloc->alloc(alloc->ctx, sizeof(*team));
    if (!team)
        return NULL;
    memset(team, 0, sizeof(*team));
    team->alloc = alloc;
    if (name && name[0])
        team->name = hu_strdup(alloc, name);
    return team;
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

hu_error_t hu_team_add_member(hu_team_t *team, uint64_t agent_id, const char *name,
                              hu_team_role_t role, uint8_t autonomy_level) {
    if (!team)
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < team->count; i++) {
        if (team->members[i].agent_id == agent_id)
            return HU_ERR_ALREADY_EXISTS;
    }
    hu_error_t err = grow_if_needed(team);
    if (err != HU_OK)
        return err;

    hu_team_member_t *m = &team->members[team->count++];
    m->agent_id = agent_id;
    m->name = name && name[0] ? hu_strdup(team->alloc, name) : NULL;
    m->role = role;
    m->autonomy_level = autonomy_level;
    m->active = true;
    return HU_OK;
}

hu_error_t hu_team_remove_member(hu_team_t *team, uint64_t agent_id) {
    if (!team)
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < team->count; i++) {
        if (team->members[i].agent_id == agent_id) {
            free_runtime_member(team->alloc, &team->members[i]);
            memmove(&team->members[i], &team->members[i + 1],
                    (team->count - 1 - i) * sizeof(hu_team_member_t));
            team->count--;
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
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

hu_error_t hu_team_list_members(hu_team_t *team, hu_team_member_t **out, size_t *count) {
    if (!team || !out || !count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *count = 0;
    if (team->count == 0)
        return HU_OK;

    hu_team_member_t *arr = (hu_team_member_t *)team->alloc->alloc(
        team->alloc->ctx, team->count * sizeof(hu_team_member_t));
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < team->count; i++) {
        arr[i].agent_id = team->members[i].agent_id;
        arr[i].name = team->members[i].name ? hu_strdup(team->alloc, team->members[i].name) : NULL;
        arr[i].role = team->members[i].role;
        arr[i].autonomy_level = team->members[i].autonomy_level;
        arr[i].active = team->members[i].active;
    }
    *out = arr;
    *count = team->count;
    return HU_OK;
}

const char *hu_team_name(const hu_team_t *team) {
    return team ? team->name : NULL;
}

size_t hu_team_member_count(const hu_team_t *team) {
    return team ? team->count : 0;
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
