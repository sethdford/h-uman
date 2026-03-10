#include "human/skillforge.h"
#include "human/core/allocator.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#endif

#define HU_SKILLFORGE_INIT_CAP 8

static void skill_free(hu_allocator_t *a, hu_skill_t *s) {
    if (!a || !s)
        return;
    if (s->name)
        a->free(a->ctx, s->name, strlen(s->name) + 1);
    if (s->description)
        a->free(a->ctx, s->description, strlen(s->description) + 1);
    if (s->parameters)
        a->free(a->ctx, s->parameters, strlen(s->parameters) + 1);
    s->name = s->description = s->parameters = NULL;
}

static hu_error_t skill_add(hu_skillforge_t *sf, const char *name, const char *desc,
                            const char *params, bool enabled) {
    if (sf->skills_len >= sf->skills_cap) {
        size_t new_cap = sf->skills_cap ? sf->skills_cap * 2 : HU_SKILLFORGE_INIT_CAP;
        hu_skill_t *n = (hu_skill_t *)sf->alloc->realloc(sf->alloc->ctx, sf->skills,
                                                         sf->skills_cap * sizeof(hu_skill_t),
                                                         new_cap * sizeof(hu_skill_t));
        if (!n)
            return HU_ERR_OUT_OF_MEMORY;
        sf->skills = n;
        sf->skills_cap = new_cap;
    }
    hu_skill_t *s = &sf->skills[sf->skills_len];
    s->name = hu_strdup(sf->alloc, name);
    s->description = desc ? hu_strdup(sf->alloc, desc) : hu_strdup(sf->alloc, "");
    s->parameters = params ? hu_strdup(sf->alloc, params) : NULL;
    s->enabled = enabled;
    if (!s->name || !s->description) {
        skill_free(sf->alloc, s);
        return HU_ERR_OUT_OF_MEMORY;
    }
    sf->skills_len++;
    return HU_OK;
}

#if !defined(HU_IS_TEST)
static hu_error_t parse_skill_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                                   char **out_name, char **out_desc, char **out_params,
                                   bool *out_enabled) {
    hu_json_value_t *root = NULL;
    hu_error_t err = hu_json_parse(alloc, json, json_len, &root);
    if (err != HU_OK)
        return err;
    if (!root || root->type != HU_JSON_OBJECT) {
        if (root)
            hu_json_free(alloc, root);
        return HU_ERR_JSON_PARSE;
    }

    const char *name = hu_json_get_string(root, "name");
    const char *desc = hu_json_get_string(root, "description");
    bool enabled = hu_json_get_bool(root, "enabled", true);

    *out_name = name ? hu_strdup(alloc, name) : NULL;
    *out_desc = desc ? hu_strdup(alloc, desc) : hu_strdup(alloc, "");
    *out_params = NULL;
    if (root->data.object.pairs) {
        for (size_t i = 0; i < root->data.object.len; i++) {
            if (strcmp(root->data.object.pairs[i].key, "parameters") == 0) {
                hu_json_value_t *v = root->data.object.pairs[i].value;
                if (v && v->type == HU_JSON_STRING && v->data.string.ptr) {
                    *out_params = hu_strdup(alloc, v->data.string.ptr);
                } else if (v) {
                    char *str = NULL;
                    size_t slen = 0;
                    if (hu_json_stringify(alloc, v, &str, &slen) == HU_OK && str) {
                        *out_params = str;
                    }
                }
                break;
            }
        }
    }
    *out_enabled = enabled;
    hu_json_free(alloc, root);

    if (!*out_name || !*out_desc) {
        if (*out_name)
            alloc->free(alloc->ctx, *out_name, strlen(*out_name) + 1);
        if (*out_desc)
            alloc->free(alloc->ctx, *out_desc, strlen(*out_desc) + 1);
        if (*out_params)
            alloc->free(alloc->ctx, *out_params, strlen(*out_params) + 1);
        return HU_ERR_JSON_PARSE;
    }
    return HU_OK;
}
#endif

#if !defined(HU_IS_TEST)
static hu_error_t discover_from_dir(hu_skillforge_t *sf, const char *dir_path) {
#ifndef _WIN32
    DIR *d = opendir(dir_path);
    if (!d)
        return HU_OK;

    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' || e->d_name[0] == '\0')
            continue;
        size_t nlen = strlen(e->d_name);
        if (nlen < 12 || strcmp(e->d_name + nlen - 11, ".skill.json") != 0)
            continue;

        char path[1024];
        int plen = snprintf(path, sizeof(path), "%s/%s", dir_path, e->d_name);
        if (plen <= 0 || (size_t)plen >= sizeof(path))
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
        hu_error_t err = parse_skill_json(sf->alloc, buf, nr, &name, &desc, &params, &enabled);
        sf->alloc->free(sf->alloc->ctx, buf, (size_t)sz + 1);
        if (err != HU_OK || !name) {
            if (name)
                sf->alloc->free(sf->alloc->ctx, name, strlen(name) + 1);
            if (desc)
                sf->alloc->free(sf->alloc->ctx, desc, strlen(desc) + 1);
            if (params)
                sf->alloc->free(sf->alloc->ctx, params, strlen(params) + 1);
            continue;
        }

        (void)skill_add(sf, name, desc, params, enabled);
        sf->alloc->free(sf->alloc->ctx, name, strlen(name) + 1);
        sf->alloc->free(sf->alloc->ctx, desc, strlen(desc) + 1);
        if (params)
            sf->alloc->free(sf->alloc->ctx, params, strlen(params) + 1);
    }
    closedir(d);
#else
    (void)sf;
    (void)dir_path;
#endif
    return HU_OK;
}
#endif

#ifdef HU_IS_TEST
static hu_error_t discover_test_data(hu_skillforge_t *sf) {
    /* Add test skills without scanning filesystem */
    hu_error_t err;
    err = skill_add(sf, "test-skill", "A test skill for unit tests", "{}", true);
    if (err != HU_OK)
        return err;
    err = skill_add(sf, "another-skill", "Another test skill", NULL, false);
    if (err != HU_OK)
        return err;
    err = skill_add(sf, "cli-helper", "CLI helper skill", "{\"prompt\": \"string\"}", true);
    if (err != HU_OK)
        return err;
    return HU_OK;
}
#endif

hu_error_t hu_skillforge_create(hu_allocator_t *alloc, hu_skillforge_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->alloc = alloc;
    out->skills_cap = HU_SKILLFORGE_INIT_CAP;
    out->skills = (hu_skill_t *)alloc->alloc(alloc->ctx, out->skills_cap * sizeof(hu_skill_t));
    if (!out->skills)
        return HU_ERR_OUT_OF_MEMORY;
    return HU_OK;
}

void hu_skillforge_destroy(hu_skillforge_t *sf) {
    if (!sf)
        return;
    for (size_t i = 0; i < sf->skills_len; i++)
        skill_free(sf->alloc, &sf->skills[i]);
    if (sf->skills)
        sf->alloc->free(sf->alloc->ctx, sf->skills, sf->skills_cap * sizeof(hu_skill_t));
    memset(sf, 0, sizeof(*sf));
}

hu_error_t hu_skillforge_discover(hu_skillforge_t *sf, const char *dir_path) {
    if (!sf || !dir_path)
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_IS_TEST
    return discover_test_data(sf);
#else
    return discover_from_dir(sf, dir_path);
#endif
}

hu_skill_t *hu_skillforge_get_skill(const hu_skillforge_t *sf, const char *name) {
    if (!sf || !name)
        return NULL;
    for (size_t i = 0; i < sf->skills_len; i++) {
        if (strcmp(sf->skills[i].name, name) == 0)
            return &sf->skills[i];
    }
    return NULL;
}

hu_error_t hu_skillforge_list_skills(const hu_skillforge_t *sf, hu_skill_t **out,
                                     size_t *out_count) {
    if (!sf || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = sf->skills;
    *out_count = sf->skills_len;
    return HU_OK;
}

hu_error_t hu_skillforge_enable(hu_skillforge_t *sf, const char *name) {
    hu_skill_t *s = hu_skillforge_get_skill(sf, name);
    if (!s)
        return HU_ERR_NOT_FOUND;
    s->enabled = true;
    return HU_OK;
}

hu_error_t hu_skillforge_disable(hu_skillforge_t *sf, const char *name) {
    hu_skill_t *s = hu_skillforge_get_skill(sf, name);
    if (!s)
        return HU_ERR_NOT_FOUND;
    s->enabled = false;
    return HU_OK;
}

hu_error_t hu_skillforge_execute(hu_allocator_t *alloc, const hu_skillforge_t *sf, const char *name,
                                 char **out_instructions) {
    if (!alloc || !sf || !name || !out_instructions)
        return HU_ERR_INVALID_ARGUMENT;
    *out_instructions = NULL;
    hu_skill_t *s = hu_skillforge_get_skill(sf, name);
    if (!s)
        return HU_ERR_NOT_FOUND;
    if (!s->description || !s->description[0]) {
        char *empty = (char *)alloc->alloc(alloc->ctx, 1);
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        empty[0] = '\0';
        *out_instructions = empty;
        return HU_OK;
    }
    *out_instructions = hu_strdup(alloc, s->description);
    if (!*out_instructions)
        return HU_ERR_OUT_OF_MEMORY;
    return HU_OK;
}

hu_error_t hu_skillforge_install(const char *name, const char *url) {
    if (!name || !name[0] || !url || !url[0])
        return HU_ERR_INVALID_ARGUMENT;
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, ".."))
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_IS_TEST
    (void)url;
    return HU_OK;
#else
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return HU_ERR_INVALID_ARGUMENT;

    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/.human/skills/%.256s.skill.json", home, name);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;

    char dir_path[1024];
    n = snprintf(dir_path, sizeof(dir_path), "%s/.human/skills", home);
    if (n <= 0 || (size_t)n >= sizeof(dir_path))
        return HU_ERR_INVALID_ARGUMENT;
#ifndef _WIN32
    mkdir(dir_path, 0755);
#endif

    hu_allocator_t alloc = hu_system_allocator();
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(&alloc, url, NULL, &resp);
    if (err != HU_OK || !resp.body) {
        hu_http_response_free(&alloc, &resp);
        return err != HU_OK ? err : HU_ERR_PROVIDER_RESPONSE;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        hu_http_response_free(&alloc, &resp);
        return HU_ERR_IO;
    }
    size_t written = fwrite(resp.body, 1, resp.body_len, f);
    fclose(f);
    hu_http_response_free(&alloc, &resp);
    if (written != resp.body_len) {
        remove(path);
        return HU_ERR_IO;
    }
    return HU_OK;
#endif
}

hu_error_t hu_skillforge_uninstall(hu_skillforge_t *sf, const char *name) {
    if (!sf || !name)
        return HU_ERR_INVALID_ARGUMENT;
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, ".."))
        return HU_ERR_INVALID_ARGUMENT;
    for (size_t i = 0; i < sf->skills_len; i++) {
        if (strcmp(sf->skills[i].name, name) == 0) {
            skill_free(sf->alloc, &sf->skills[i]);
            if (i + 1 < sf->skills_len) {
                memmove(&sf->skills[i], &sf->skills[i + 1],
                        (sf->skills_len - i - 1) * sizeof(hu_skill_t));
            }
            sf->skills_len--;

            const char *home = getenv("HOME");
            if (home) {
                char path[1024];
                int n = snprintf(path, sizeof(path), "%s/.human/skills/%.256s.skill.json", home,
                                 name);
                if (n > 0 && (size_t)n < sizeof(path))
                    remove(path);
            }
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
}
