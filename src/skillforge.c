#include "seaclaw/skillforge.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdio.h>

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

#define SC_SKILLFORGE_INIT_CAP 8

static void skill_free(sc_allocator_t *a, sc_skill_t *s) {
    if (!a || !s) return;
    if (s->name) a->free(a->ctx, s->name, strlen(s->name) + 1);
    if (s->description) a->free(a->ctx, s->description, strlen(s->description) + 1);
    if (s->parameters) a->free(a->ctx, s->parameters, strlen(s->parameters) + 1);
    s->name = s->description = s->parameters = NULL;
}

static sc_error_t skill_add(sc_skillforge_t *sf, const char *name,
    const char *desc, const char *params, bool enabled) {
    if (sf->skills_len >= sf->skills_cap) {
        size_t new_cap = sf->skills_cap ? sf->skills_cap * 2 : SC_SKILLFORGE_INIT_CAP;
        sc_skill_t *n = (sc_skill_t *)sf->alloc->realloc(sf->alloc->ctx,
            sf->skills, sf->skills_cap * sizeof(sc_skill_t), new_cap * sizeof(sc_skill_t));
        if (!n) return SC_ERR_OUT_OF_MEMORY;
        sf->skills = n;
        sf->skills_cap = new_cap;
    }
    sc_skill_t *s = &sf->skills[sf->skills_len];
    s->name = sc_strdup(sf->alloc, name);
    s->description = desc ? sc_strdup(sf->alloc, desc) : sc_strdup(sf->alloc, "");
    s->parameters = params ? sc_strdup(sf->alloc, params) : NULL;
    s->enabled = enabled;
    if (!s->name || !s->description) {
        skill_free(sf->alloc, s);
        return SC_ERR_OUT_OF_MEMORY;
    }
    sf->skills_len++;
    return SC_OK;
}

static sc_error_t parse_skill_json(sc_allocator_t *alloc, const char *json, size_t json_len,
    char **out_name, char **out_desc, char **out_params, bool *out_enabled) {
    sc_json_value_t *root = NULL;
    sc_error_t err = sc_json_parse(alloc, json, json_len, &root);
    if (err != SC_OK) return err;
    if (!root || root->type != SC_JSON_OBJECT) {
        if (root) sc_json_free(alloc, root);
        return SC_ERR_JSON_PARSE;
    }

    const char *name = sc_json_get_string(root, "name");
    const char *desc = sc_json_get_string(root, "description");
    bool enabled = sc_json_get_bool(root, "enabled", true);

    *out_name = name ? sc_strdup(alloc, name) : NULL;
    *out_desc = desc ? sc_strdup(alloc, desc) : sc_strdup(alloc, "");
    *out_params = NULL;
    if (root->data.object.pairs) {
        for (size_t i = 0; i < root->data.object.len; i++) {
            if (strcmp(root->data.object.pairs[i].key, "parameters") == 0) {
                sc_json_value_t *v = root->data.object.pairs[i].value;
                if (v && v->type == SC_JSON_STRING && v->data.string.ptr) {
                    *out_params = sc_strdup(alloc, v->data.string.ptr);
                } else if (v) {
                    char *str = NULL;
                    size_t slen = 0;
                    if (sc_json_stringify(alloc, v, &str, &slen) == SC_OK && str) {
                        *out_params = str;
                    }
                }
                break;
            }
        }
    }
    *out_enabled = enabled;
    sc_json_free(alloc, root);

    if (!*out_name || !*out_desc) {
        if (*out_name) alloc->free(alloc->ctx, *out_name, strlen(*out_name) + 1);
        if (*out_desc) alloc->free(alloc->ctx, *out_desc, strlen(*out_desc) + 1);
        if (*out_params) alloc->free(alloc->ctx, *out_params, strlen(*out_params) + 1);
        return SC_ERR_JSON_PARSE;
    }
    return SC_OK;
}

#if !defined(SC_IS_TEST)
static sc_error_t discover_from_dir(sc_skillforge_t *sf, const char *dir_path) {
#ifndef _WIN32
    DIR *d = opendir(dir_path);
    if (!d) return SC_OK;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || e->d_name[0] == '\0') continue;
        size_t nlen = strlen(e->d_name);
        if (nlen < 12 || strcmp(e->d_name + nlen - 11, ".skill.json") != 0)
            continue;

        char path[1024];
        int plen = snprintf(path, sizeof(path), "%s/%s", dir_path, e->d_name);
        if (plen <= 0 || (size_t)plen >= sizeof(path)) continue;

        FILE *f = fopen(path, "rb");
        if (!f) continue;

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz <= 0 || sz > 65536) {
            fclose(f);
            continue;
        }
        char *buf = (char *)sf->alloc->alloc(sf->alloc->ctx, (size_t)sz + 1);
        if (!buf) {
            fclose(f);
            continue;
        }
        size_t nr = fread(buf, 1, (size_t)sz, f);
        fclose(f);
        buf[nr] = '\0';

        char *name = NULL, *desc = NULL, *params = NULL;
        bool enabled = true;
        sc_error_t err = parse_skill_json(sf->alloc, buf, nr, &name, &desc, &params, &enabled);
        sf->alloc->free(sf->alloc->ctx, buf, (size_t)sz + 1);
        if (err != SC_OK || !name) {
            if (name) sf->alloc->free(sf->alloc->ctx, name, strlen(name) + 1);
            if (desc) sf->alloc->free(sf->alloc->ctx, desc, strlen(desc) + 1);
            if (params) sf->alloc->free(sf->alloc->ctx, params, strlen(params) + 1);
            continue;
        }

        (void)skill_add(sf, name, desc, params, enabled);
        sf->alloc->free(sf->alloc->ctx, name, strlen(name) + 1);
        sf->alloc->free(sf->alloc->ctx, desc, strlen(desc) + 1);
        if (params) sf->alloc->free(sf->alloc->ctx, params, strlen(params) + 1);
    }
    closedir(d);
#else
    (void)sf;
    (void)dir_path;
#endif
    return SC_OK;
}
#endif

#ifdef SC_IS_TEST
static sc_error_t discover_test_data(sc_skillforge_t *sf) {
    /* Add test skills without scanning filesystem */
    sc_error_t err;
    err = skill_add(sf, "test-skill", "A test skill for unit tests", "{}", true);
    if (err != SC_OK) return err;
    err = skill_add(sf, "another-skill", "Another test skill", NULL, false);
    if (err != SC_OK) return err;
    err = skill_add(sf, "cli-helper", "CLI helper skill", "{\"prompt\": \"string\"}", true);
    if (err != SC_OK) return err;
    return SC_OK;
}
#endif

sc_error_t sc_skillforge_create(sc_allocator_t *alloc, sc_skillforge_t *out) {
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->alloc = alloc;
    out->skills_cap = SC_SKILLFORGE_INIT_CAP;
    out->skills = (sc_skill_t *)alloc->alloc(alloc->ctx,
        out->skills_cap * sizeof(sc_skill_t));
    if (!out->skills) return SC_ERR_OUT_OF_MEMORY;
    return SC_OK;
}

void sc_skillforge_destroy(sc_skillforge_t *sf) {
    if (!sf) return;
    for (size_t i = 0; i < sf->skills_len; i++)
        skill_free(sf->alloc, &sf->skills[i]);
    if (sf->skills)
        sf->alloc->free(sf->alloc->ctx, sf->skills,
            sf->skills_cap * sizeof(sc_skill_t));
    memset(sf, 0, sizeof(*sf));
}

sc_error_t sc_skillforge_discover(sc_skillforge_t *sf, const char *dir_path) {
    if (!sf || !dir_path) return SC_ERR_INVALID_ARGUMENT;

#ifdef SC_IS_TEST
    return discover_test_data(sf);
#else
    return discover_from_dir(sf, dir_path);
#endif
}

sc_skill_t *sc_skillforge_get_skill(const sc_skillforge_t *sf, const char *name) {
    if (!sf || !name) return NULL;
    for (size_t i = 0; i < sf->skills_len; i++) {
        if (strcmp(sf->skills[i].name, name) == 0)
            return &sf->skills[i];
    }
    return NULL;
}

sc_error_t sc_skillforge_list_skills(const sc_skillforge_t *sf,
    sc_skill_t **out, size_t *out_count) {
    if (!sf || !out || !out_count) return SC_ERR_INVALID_ARGUMENT;
    *out = sf->skills;
    *out_count = sf->skills_len;
    return SC_OK;
}

sc_error_t sc_skillforge_enable(sc_skillforge_t *sf, const char *name) {
    sc_skill_t *s = sc_skillforge_get_skill(sf, name);
    if (!s) return SC_ERR_NOT_FOUND;
    s->enabled = true;
    return SC_OK;
}

sc_error_t sc_skillforge_disable(sc_skillforge_t *sf, const char *name) {
    sc_skill_t *s = sc_skillforge_get_skill(sf, name);
    if (!s) return SC_ERR_NOT_FOUND;
    s->enabled = false;
    return SC_OK;
}
