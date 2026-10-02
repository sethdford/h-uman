/*
 * Agent Registry — discovers named agent definitions from JSON files on disk
 * and provides runtime lookup by name and capability matching.
 */
#include "human/agent/registry.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

static char *dup_str(hu_allocator_t *alloc, const char *s) {
    return s ? hu_strdup(alloc, s) : NULL;
}

static const char **dup_string_array(hu_allocator_t *alloc, const char **src, size_t count) {
    if (!src || count == 0)
        return NULL;
    const char **arr = (const char **)alloc->alloc(alloc->ctx, count * sizeof(const char *));
    if (!arr)
        return NULL;
    for (size_t i = 0; i < count; i++)
        arr[i] = src[i] ? hu_strdup(alloc, src[i]) : NULL;
    return arr;
}

void hu_named_agent_config_free(hu_allocator_t *alloc, hu_named_agent_config_t *cfg) {
    if (!alloc || !cfg)
        return;
    if (cfg->name)
        alloc->free(alloc->ctx, (void *)cfg->name, strlen(cfg->name) + 1);
    if (cfg->provider)
        alloc->free(alloc->ctx, (void *)cfg->provider, strlen(cfg->provider) + 1);
    if (cfg->model)
        alloc->free(alloc->ctx, (void *)cfg->model, strlen(cfg->model) + 1);
    if (cfg->persona)
        alloc->free(alloc->ctx, (void *)cfg->persona, strlen(cfg->persona) + 1);
    if (cfg->system_prompt)
        alloc->free(alloc->ctx, (void *)cfg->system_prompt, strlen(cfg->system_prompt) + 1);
    if (cfg->role)
        alloc->free(alloc->ctx, (void *)cfg->role, strlen(cfg->role) + 1);
    if (cfg->description)
        alloc->free(alloc->ctx, (void *)cfg->description, strlen(cfg->description) + 1);
    if (cfg->capabilities)
        alloc->free(alloc->ctx, (void *)cfg->capabilities, strlen(cfg->capabilities) + 1);
    for (size_t i = 0; i < cfg->enabled_tools_count; i++) {
        if (cfg->enabled_tools[i])
            alloc->free(alloc->ctx, (void *)cfg->enabled_tools[i],
                        strlen(cfg->enabled_tools[i]) + 1);
    }
    if (cfg->enabled_tools)
        alloc->free(alloc->ctx, (void *)cfg->enabled_tools,
                    cfg->enabled_tools_count * sizeof(const char *));
    for (size_t i = 0; i < cfg->enabled_skills_count; i++) {
        if (cfg->enabled_skills[i])
            alloc->free(alloc->ctx, (void *)cfg->enabled_skills[i],
                        strlen(cfg->enabled_skills[i]) + 1);
    }
    if (cfg->enabled_skills)
        alloc->free(alloc->ctx, (void *)cfg->enabled_skills,
                    cfg->enabled_skills_count * sizeof(const char *));
    memset(cfg, 0, sizeof(*cfg));
}

static hu_error_t copy_config(hu_allocator_t *alloc, const hu_named_agent_config_t *src,
                              hu_named_agent_config_t *dst) {
    memset(dst, 0, sizeof(*dst));
    dst->name = dup_str(alloc, src->name);
    dst->provider = dup_str(alloc, src->provider);
    dst->model = dup_str(alloc, src->model);
    dst->persona = dup_str(alloc, src->persona);
    dst->system_prompt = dup_str(alloc, src->system_prompt);
    dst->role = dup_str(alloc, src->role);
    dst->description = dup_str(alloc, src->description);
    dst->capabilities = dup_str(alloc, src->capabilities);
    dst->enabled_tools = dup_string_array(alloc, src->enabled_tools, src->enabled_tools_count);
    dst->enabled_tools_count = src->enabled_tools_count;
    dst->enabled_skills = dup_string_array(alloc, src->enabled_skills, src->enabled_skills_count);
    dst->enabled_skills_count = src->enabled_skills_count;
    dst->autonomy_level = src->autonomy_level;
    dst->temperature = src->temperature;
    dst->budget_usd = src->budget_usd;
    dst->max_iterations = src->max_iterations;
    dst->is_default = src->is_default;
    if (src->name && !dst->name)
        return HU_ERR_OUT_OF_MEMORY;
    return HU_OK;
}

/* Parse a JSON string array into allocated const char** + count. */
static hu_error_t parse_string_array(hu_allocator_t *alloc, const hu_json_value_t *root,
                                     const char *key, const char ***out, size_t *out_count) {
    *out = NULL;
    *out_count = 0;
    hu_json_value_t *arr = hu_json_object_get(root, key);
    if (!arr || arr->type != HU_JSON_ARRAY || arr->data.array.len == 0)
        return HU_OK;
    size_t n = arr->data.array.len;
    const char **result = (const char **)alloc->alloc(alloc->ctx, n * sizeof(const char *));
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    for (size_t i = 0; i < n; i++) {
        hu_json_value_t *item = arr->data.array.items[i];
        if (item && item->type == HU_JSON_STRING && item->data.string.ptr)
            result[i] = hu_strdup(alloc, item->data.string.ptr);
        else
            result[i] = NULL;
    }
    *out = result;
    *out_count = n;
    return HU_OK;
}

hu_error_t hu_agent_registry_parse_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                                        hu_named_agent_config_t *out) {
    if (!alloc || !json || !out)
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK)
        return err;
    if (!root || root->type != HU_JSON_OBJECT) {
        if (root)
            hu_json_free(alloc, root);
        return HU_ERR_JSON_PARSE;
    }

    memset(out, 0, sizeof(*out));

    const char *s;
    s = hu_json_get_string(root, "name");
    if (s)
        out->name = hu_strdup(alloc, s);
    s = hu_json_get_string(root, "provider");
    if (s)
        out->provider = hu_strdup(alloc, s);
    s = hu_json_get_string(root, "model");
    if (s)
        out->model = hu_strdup(alloc, s);
    s = hu_json_get_string(root, "persona");
    if (s)
        out->persona = hu_strdup(alloc, s);
    s = hu_json_get_string(root, "system_prompt");
    if (s)
        out->system_prompt = hu_strdup(alloc, s);
    s = hu_json_get_string(root, "role");
    if (s)
        out->role = hu_strdup(alloc, s);
    s = hu_json_get_string(root, "description");
    if (s)
        out->description = hu_strdup(alloc, s);
    s = hu_json_get_string(root, "capabilities");
    if (s)
        out->capabilities = hu_strdup(alloc, s);

    out->autonomy_level = (uint8_t)hu_json_get_number(root, "autonomy_level", 0);
    out->temperature = hu_json_get_number(root, "temperature", 0.0);
    out->budget_usd = hu_json_get_number(root, "budget_usd", 0.0);
    out->max_iterations = (uint32_t)hu_json_get_number(root, "max_iterations", 0);
    out->is_default = hu_json_get_bool(root, "is_default", false);

    parse_string_array(alloc, root, "enabled_tools", &out->enabled_tools,
                       &out->enabled_tools_count);
    parse_string_array(alloc, root, "enabled_skills", &out->enabled_skills,
                       &out->enabled_skills_count);

    hu_json_free(alloc, root);

    if (!out->name) {
        hu_named_agent_config_free(alloc, out);
        return HU_ERR_JSON_PARSE;
    }
    return HU_OK;
}

hu_error_t hu_agent_registry_create(hu_allocator_t *alloc, hu_agent_registry_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->alloc = alloc;
    return HU_OK;
}

void hu_agent_registry_destroy(hu_agent_registry_t *reg) {
    if (!reg || !reg->alloc)
        return;
    for (size_t i = 0; i < reg->count; i++)
        hu_named_agent_config_free(reg->alloc, &reg->agents[i]);
    reg->count = 0;
}

hu_error_t hu_agent_registry_register(hu_agent_registry_t *reg,
                                      const hu_named_agent_config_t *cfg) {
    if (!reg || !cfg || !cfg->name)
        return HU_ERR_INVALID_ARGUMENT;
    if (reg->count >= HU_AGENT_REGISTRY_MAX)
        return HU_ERR_INVALID_ARGUMENT;

    /* Reject duplicate names */
    for (size_t i = 0; i < reg->count; i++) {
        if (reg->agents[i].name && strcmp(reg->agents[i].name, cfg->name) == 0)
            return HU_ERR_ALREADY_EXISTS;
    }

    return copy_config(reg->alloc, cfg, &reg->agents[reg->count++]);
}

const hu_named_agent_config_t *hu_agent_registry_get(const hu_agent_registry_t *reg,
                                                     const char *name) {
    if (!reg || !name)
        return NULL;
    for (size_t i = 0; i < reg->count; i++) {
        if (reg->agents[i].name && strcmp(reg->agents[i].name, name) == 0)
            return &reg->agents[i];
    }
    return NULL;
}

hu_error_t hu_agent_registry_find_by_capability(const hu_agent_registry_t *reg,
                                                const char *capability,
                                                const hu_named_agent_config_t **out, size_t max_out,
                                                size_t *count) {
    if (!reg || !capability || !out || !count)
        return HU_ERR_INVALID_ARGUMENT;
    *count = 0;
    size_t cap_len = strlen(capability);
    for (size_t i = 0; i < reg->count && *count < max_out; i++) {
        const char *caps = reg->agents[i].capabilities;
        if (!caps)
            continue;
        const char *p = caps;
        while (*p) {
            while (*p == ',' || *p == ' ')
                p++;
            const char *start = p;
            while (*p && *p != ',' && *p != ' ')
                p++;
            size_t len = (size_t)(p - start);
            if (len == cap_len && memcmp(start, capability, len) == 0) {
                out[(*count)++] = &reg->agents[i];
                break;
            }
        }
    }
    return HU_OK;
}

const hu_named_agent_config_t *hu_agent_registry_get_default(const hu_agent_registry_t *reg) {
    if (!reg || reg->count == 0)
        return NULL;
    for (size_t i = 0; i < reg->count; i++) {
        if (reg->agents[i].is_default)
            return &reg->agents[i];
    }
    return &reg->agents[0];
}

#if !defined(HU_IS_TEST)
hu_error_t hu_agent_registry_discover(hu_agent_registry_t *reg, const char *dir_path) {
    if (!reg || !dir_path)
        return HU_ERR_INVALID_ARGUMENT;
    /* reload passes reg->discover_dir itself; snprintf onto its own source is UB. */
    if (dir_path != reg->discover_dir)
        snprintf(reg->discover_dir, sizeof(reg->discover_dir), "%s", dir_path);
#ifndef _WIN32
    DIR *d = opendir(dir_path);
    if (!d)
        return HU_OK; /* no agents dir is fine */
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        size_t nlen = strlen(entry->d_name);
        if (nlen < 6 || strcmp(entry->d_name + nlen - 5, ".json") != 0)
            continue;
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s/%s", dir_path, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path))
            continue;
        FILE *f = fopen(path, "rb");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > 65536) {
            fclose(f);
            continue;
        }
        char *buf = (char *)reg->alloc->alloc(reg->alloc->ctx, (size_t)sz + 1);
        if (!buf) {
            fclose(f);
            continue;
        }
        size_t rd = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        buf[rd] = '\0';

        hu_named_agent_config_t cfg = {0};
        hu_error_t err = hu_agent_registry_parse_json(reg->alloc, buf, rd, &cfg);
        reg->alloc->free(reg->alloc->ctx, buf, (size_t)sz + 1);
        if (err == HU_OK)
            hu_agent_registry_register(reg, &cfg);
        hu_named_agent_config_free(reg->alloc, &cfg);
    }
    closedir(d);
#endif
    return HU_OK;
}
#else
hu_error_t hu_agent_registry_discover(hu_agent_registry_t *reg, const char *dir_path) {
    if (!reg || !dir_path)
        return HU_ERR_INVALID_ARGUMENT;
    /* reload passes reg->discover_dir itself; snprintf onto its own source is UB. */
    if (dir_path != reg->discover_dir)
        snprintf(reg->discover_dir, sizeof(reg->discover_dir), "%s", dir_path);
    return HU_OK;
}
#endif

hu_error_t hu_agent_registry_reload(hu_agent_registry_t *reg) {
    if (!reg)
        return HU_ERR_INVALID_ARGUMENT;
    if (reg->discover_dir[0] == '\0')
        return HU_OK;
    for (size_t i = 0; i < reg->count; i++)
        hu_named_agent_config_free(reg->alloc, &reg->agents[i]);
    reg->count = 0;
    return hu_agent_registry_discover(reg, reg->discover_dir);
}
