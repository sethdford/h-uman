#include "human/skillforge.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/json.h"
#include "human/core/log.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifndef _WIN32
#include <dirent.h>
#endif

#define HU_SKILLFORGE_INIT_CAP         8
#define HU_SKILLFORGE_FILE_MAX         65536
#define HU_SKILLFORGE_FRONTMATTER_SCAN 4096

static void skill_free(hu_allocator_t *a, hu_skill_t *s) {
    if (!a || !s)
        return;
    if (s->name)
        a->free(a->ctx, s->name, strlen(s->name) + 1);
    if (s->description)
        a->free(a->ctx, s->description, strlen(s->description) + 1);
    if (s->command)
        a->free(a->ctx, s->command, strlen(s->command) + 1);
    if (s->parameters)
        a->free(a->ctx, s->parameters, strlen(s->parameters) + 1);
    if (s->skill_dir)
        a->free(a->ctx, s->skill_dir, strlen(s->skill_dir) + 1);
    if (s->instructions_path)
        a->free(a->ctx, s->instructions_path, strlen(s->instructions_path) + 1);
    s->name = s->description = s->command = s->parameters = s->skill_dir = s->instructions_path =
        NULL;
}

static hu_error_t skill_add(hu_skillforge_t *sf, const char *name, const char *desc,
                            const char *command, const char *params, bool enabled,
                            const char *skill_dir, const char *instructions_path) {
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
    s->command = command ? hu_strdup(sf->alloc, command) : NULL;
    s->parameters = params ? hu_strdup(sf->alloc, params) : NULL;
    s->skill_dir = skill_dir ? hu_strdup(sf->alloc, skill_dir) : NULL;
    s->instructions_path = instructions_path ? hu_strdup(sf->alloc, instructions_path) : NULL;
    s->enabled = enabled;
    if (!s->name || !s->description || (skill_dir && !s->skill_dir) ||
        (instructions_path && !s->instructions_path)) {
        skill_free(sf->alloc, s);
        return HU_ERR_OUT_OF_MEMORY;
    }
    sf->skills_len++;
    return HU_OK;
}

#if !defined(HU_IS_TEST)
static hu_error_t parse_skill_json(hu_allocator_t *alloc, const char *json, size_t json_len,
                                   char **out_name, char **out_desc, char **out_command,
                                   char **out_params, bool *out_enabled) {
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
    const char *cmd = hu_json_get_string(root, "command");
    bool enabled = hu_json_get_bool(root, "enabled", true);

    *out_name = name ? hu_strdup(alloc, name) : NULL;
    *out_desc = desc ? hu_strdup(alloc, desc) : hu_strdup(alloc, "");
    *out_command = cmd ? hu_strdup(alloc, cmd) : NULL;
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
        if (*out_command)
            alloc->free(alloc->ctx, *out_command, strlen(*out_command) + 1);
        if (*out_params)
            alloc->free(alloc->ctx, *out_params, strlen(*out_params) + 1);
        return HU_ERR_JSON_PARSE;
    }
    return HU_OK;
}
#endif

static int skillforge_resource_name_ok(const char *name) {
    if (!name || !name[0])
        return 0;
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, ".."))
        return 0;
    return 1;
}

#if !defined(HU_IS_TEST)

static hu_error_t read_file_capped(hu_allocator_t *alloc, const char *path, size_t max_bytes,
                                   char **out_buf, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return HU_ERR_IO;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || (size_t)sz > max_bytes) {
        fclose(f);
        return HU_ERR_INVALID_ARGUMENT;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\0';
    *out_buf = buf;
    *out_len = nr;
    return HU_OK;
}

static char *skill_md_body_after_frontmatter(hu_allocator_t *alloc, const char *data, size_t len,
                                             size_t *body_len) {
    if (len < 4 || data[0] != '-' || data[1] != '-' || data[2] != '-')
        goto whole_file;
    size_t pos = 3;
    if (pos < len && data[pos] == '\r')
        pos++;
    if (pos < len && data[pos] == '\n')
        pos++;
    while (pos < len) {
        size_t line_start = pos;
        if (line_start + 2 < len && data[line_start] == '-' && data[line_start + 1] == '-' &&
            data[line_start + 2] == '-') {
            size_t after = line_start + 3;
            if (after >= len || data[after] == '\n' || data[after] == '\r') {
                if (after < len && data[after] == '\r')
                    after++;
                if (after < len && data[after] == '\n')
                    after++;
                while (after < len && (data[after] == ' ' || data[after] == '\t' ||
                                       data[after] == '\r' || data[after] == '\n'))
                    after++;
                *body_len = len - after;
                return hu_strndup(alloc, data + after, *body_len);
            }
        }
        while (pos < len && data[pos] != '\n')
            pos++;
        if (pos < len)
            pos++;
    }
whole_file:
    *body_len = len;
    return hu_strndup(alloc, data, len);
}

static void trim_line_value(const char *start, const char *end, char *dst, size_t dst_cap) {
    const char *v = start;
    while (v < end && (*v == ' ' || *v == '\t'))
        v++;
    const char *e = end;
    while (e > v && (e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
        e--;
    size_t n = (size_t)(e - v);
    if (n >= dst_cap)
        n = dst_cap - 1;
    memcpy(dst, v, n);
    dst[n] = '\0';
    if (n >= 2 &&
        ((dst[0] == '"' && dst[n - 1] == '"') || (dst[0] == '\'' && dst[n - 1] == '\''))) {
        dst[n - 1] = '\0';
        memmove(dst, dst + 1, n - 1);
    }
}

static void skill_md_apply_frontmatter(hu_allocator_t *alloc, const char *path, char **name,
                                       char **desc) {
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    char buf[HU_SKILLFORGE_FRONTMATTER_SCAN];
    size_t nr = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[nr] = '\0';
    if (nr < 4 || buf[0] != '-' || buf[1] != '-' || buf[2] != '-')
        return;

    char *close = strstr(buf + 3, "\n---");
    if (!close)
        close = strstr(buf + 3, "\r\n---");
    if (!close)
        return;
    *close = '\0';

    char *p = buf + 3;
    while (*p == '\n' || *p == '\r')
        p++;

    char new_name[256];
    char new_desc[512];
    new_name[0] = new_desc[0] = '\0';

    while (*p) {
        char *nl = strchr(p, '\n');
        if (!nl)
            nl = p + strlen(p);
        if ((size_t)(nl - p) >= 5 && strncmp(p, "name:", 5) == 0)
            trim_line_value(p + 5, nl, new_name, sizeof(new_name));
        else if ((size_t)(nl - p) >= 12 && strncmp(p, "description:", 12) == 0)
            trim_line_value(p + 12, nl, new_desc, sizeof(new_desc));
        if (*nl)
            p = nl + 1;
        else
            break;
    }

    if (new_name[0]) {
        char *dup = hu_strdup(alloc, new_name);
        if (dup) {
            alloc->free(alloc->ctx, *name, strlen(*name) + 1);
            *name = dup;
        }
    }
    if (new_desc[0]) {
        char *dup = hu_strdup(alloc, new_desc);
        if (dup) {
            alloc->free(alloc->ctx, *desc, strlen(*desc) + 1);
            *desc = dup;
        }
    }
}

static hu_error_t load_skill_file(hu_skillforge_t *sf, const char *json_path,
                                  const char *skill_dir_for_resources, const char *skill_md_path) {
    FILE *f = fopen(json_path, "rb");
    if (!f)
        return HU_ERR_IO;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > HU_SKILLFORGE_FILE_MAX) {
        fclose(f);
        return HU_ERR_INVALID_ARGUMENT;
    }
    char *buf = (char *)sf->alloc->alloc(sf->alloc->ctx, (size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return HU_ERR_OUT_OF_MEMORY;
    }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[nr] = '\0';

    char *name = NULL, *desc = NULL, *command = NULL, *params = NULL;
    bool enabled = true;
    hu_error_t err =
        parse_skill_json(sf->alloc, buf, nr, &name, &desc, &command, &params, &enabled);
    sf->alloc->free(sf->alloc->ctx, buf, (size_t)sz + 1);
    if (err != HU_OK || !name) {
        if (name)
            sf->alloc->free(sf->alloc->ctx, name, strlen(name) + 1);
        if (desc)
            sf->alloc->free(sf->alloc->ctx, desc, strlen(desc) + 1);
        if (command)
            sf->alloc->free(sf->alloc->ctx, command, strlen(command) + 1);
        if (params)
            sf->alloc->free(sf->alloc->ctx, params, strlen(params) + 1);
        return err;
    }

    const char *instr_path_arg = NULL;
    if (skill_md_path && skill_md_path[0]) {
        struct stat st_md;
        if (stat(skill_md_path, &st_md) == 0 && S_ISREG(st_md.st_mode)) {
            skill_md_apply_frontmatter(sf->alloc, skill_md_path, &name, &desc);
            instr_path_arg = skill_md_path;
        }
    }

    hu_error_t add_err = skill_add(sf, name, desc, command, params, enabled,
                                   skill_dir_for_resources, instr_path_arg);
    if (add_err != HU_OK)
        hu_log_error("skillforge", NULL, "failed to add skill from disk: %s",
                     hu_error_string(add_err));
    sf->alloc->free(sf->alloc->ctx, name, strlen(name) + 1);
    sf->alloc->free(sf->alloc->ctx, desc, strlen(desc) + 1);
    if (command)
        sf->alloc->free(sf->alloc->ctx, command, strlen(command) + 1);
    if (params)
        sf->alloc->free(sf->alloc->ctx, params, strlen(params) + 1);
    return HU_OK;
}

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

        /* *.skill.json — flat files; optional stem/SKILL.md */
        if (nlen >= 12 && strcmp(e->d_name + nlen - 11, ".skill.json") == 0) {
            char path[1024];
            int plen = snprintf(path, sizeof(path), "%s/%s", dir_path, e->d_name);
            if (plen > 0 && (size_t)plen < sizeof(path)) {
                size_t stem_len = nlen - 11;
                char skill_md_path[1024];
                int mdlen = -1;
                if (stem_len > 0)
                    mdlen = snprintf(skill_md_path, sizeof(skill_md_path), "%s/%.*s/SKILL.md",
                                     dir_path, (int)stem_len, e->d_name);
                if (mdlen > 0 && (size_t)mdlen < sizeof(skill_md_path))
                    load_skill_file(sf, path, NULL, skill_md_path);
                else
                    load_skill_file(sf, path, NULL, NULL);
            }
            continue;
        }

        /* subdirectory/manifest.json — skill_write format + SKILL.md */
        char subpath[1024];
        int n = snprintf(subpath, sizeof(subpath), "%s/%s/manifest.json", dir_path, e->d_name);
        if (n > 0 && (size_t)n < sizeof(subpath)) {
            struct stat st;
            if (stat(subpath, &st) == 0 && S_ISREG(st.st_mode)) {
                char skill_dir_buf[1024];
                int sdlen =
                    snprintf(skill_dir_buf, sizeof(skill_dir_buf), "%s/%s", dir_path, e->d_name);
                char skill_md_path[1024];
                int mdlen = snprintf(skill_md_path, sizeof(skill_md_path), "%s/%s/SKILL.md",
                                     dir_path, e->d_name);
                if (sdlen > 0 && (size_t)sdlen < sizeof(skill_dir_buf) && mdlen > 0 &&
                    (size_t)mdlen < sizeof(skill_md_path))
                    load_skill_file(sf, subpath, skill_dir_buf, skill_md_path);
                else
                    load_skill_file(sf, subpath, NULL, NULL);
            }
        }
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
    hu_error_t err;
    err = skill_add(sf, "test-skill", "A test skill for unit tests", "echo test", "{}", true, NULL,
                    NULL);
    if (err != HU_OK)
        return err;
    err = skill_add(sf, "another-skill", "Another test skill", NULL, NULL, false, NULL, NULL);
    if (err != HU_OK)
        return err;
    err = skill_add(sf, "cli-helper", "CLI helper skill", "human help", "{\"prompt\": \"string\"}",
                    true, NULL, NULL);
    if (err != HU_OK)
        return err;
    err = skill_add(sf, "skill-md-mock", "Short catalog desc for mock SKILL.md", NULL, NULL, true,
                    "/mock/skill-dir", HU_SKILLFORGE_TEST_INSTRUCTIONS_PATH);
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

hu_error_t hu_skillforge_load_instructions(hu_allocator_t *alloc, const hu_skill_t *skill,
                                           char **out_instructions, size_t *out_len) {
    if (!alloc || !skill || !out_instructions)
        return HU_ERR_INVALID_ARGUMENT;
    *out_instructions = NULL;
    if (out_len)
        *out_len = 0;

#ifdef HU_IS_TEST
    if (skill->instructions_path &&
        strcmp(skill->instructions_path, HU_SKILLFORGE_TEST_INSTRUCTIONS_PATH) == 0) {
        static const char k_mock[] =
            "# Mock SKILL.md\n\nFull instruction body for tests.\n## Step\nDo the thing.\n";
        *out_instructions = hu_strdup(alloc, k_mock);
        if (!*out_instructions)
            return HU_ERR_OUT_OF_MEMORY;
        if (out_len)
            *out_len = strlen(*out_instructions);
        return HU_OK;
    }
    const char *fb = skill->description ? skill->description : "";
    *out_instructions = hu_strdup(alloc, fb);
    if (!*out_instructions)
        return HU_ERR_OUT_OF_MEMORY;
    if (out_len)
        *out_len = strlen(*out_instructions);
    return HU_OK;
#else
    if (skill->instructions_path && skill->instructions_path[0]) {
        char *raw = NULL;
        size_t raw_len = 0;
        hu_error_t rerr = read_file_capped(alloc, skill->instructions_path, HU_SKILLFORGE_FILE_MAX,
                                           &raw, &raw_len);
        if (rerr == HU_OK && raw) {
            size_t body_len = 0;
            char *body = skill_md_body_after_frontmatter(alloc, raw, raw_len, &body_len);
            alloc->free(alloc->ctx, raw, raw_len + 1);
            if (!body) {
                const char *fb = skill->description ? skill->description : "";
                *out_instructions = hu_strdup(alloc, fb);
            } else {
                *out_instructions = body;
                body_len = strlen(body);
            }
            if (!*out_instructions)
                return HU_ERR_OUT_OF_MEMORY;
            if (out_len)
                *out_len = strlen(*out_instructions);
            return HU_OK;
        }
    }
    const char *fb = skill->description ? skill->description : "";
    if (!fb[0]) {
        char *empty = (char *)alloc->alloc(alloc->ctx, 1);
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        empty[0] = '\0';
        *out_instructions = empty;
        if (out_len)
            *out_len = 0;
        return HU_OK;
    }
    *out_instructions = hu_strdup(alloc, fb);
    if (!*out_instructions)
        return HU_ERR_OUT_OF_MEMORY;
    if (out_len)
        *out_len = strlen(*out_instructions);
    return HU_OK;
#endif
}

hu_error_t hu_skillforge_read_resource(hu_allocator_t *alloc, const hu_skill_t *skill,
                                       const char *resource_name, char **out_content,
                                       size_t *out_len) {
    if (!alloc || !skill || !resource_name || !out_content)
        return HU_ERR_INVALID_ARGUMENT;
    *out_content = NULL;
    size_t len_store = 0;
    size_t *len_out = out_len ? out_len : &len_store;
    *len_out = 0;
    if (!skillforge_resource_name_ok(resource_name))
        return HU_ERR_INVALID_ARGUMENT;
    if (!skill->skill_dir || !skill->skill_dir[0])
        return HU_ERR_INVALID_ARGUMENT;

#ifdef HU_IS_TEST
    if (strcmp(resource_name, "fixture.txt") == 0) {
        static const char k_fix[] = "fixture resource body\n";
        *out_content = hu_strdup(alloc, k_fix);
        if (!*out_content)
            return HU_ERR_OUT_OF_MEMORY;
        *len_out = strlen(*out_content);
        return HU_OK;
    }
    (void)skill;
    return HU_ERR_NOT_FOUND;
#else
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", skill->skill_dir, resource_name);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return HU_ERR_NOT_FOUND;
    return read_file_capped(alloc, path, HU_SKILLFORGE_FILE_MAX, out_content, len_out);
#endif
}

hu_error_t hu_skillforge_execute(hu_allocator_t *alloc, const hu_skillforge_t *sf, const char *name,
                                 char **out_instructions) {
    if (!alloc || !sf || !name || !out_instructions)
        return HU_ERR_INVALID_ARGUMENT;
    *out_instructions = NULL;
    hu_skill_t *s = hu_skillforge_get_skill(sf, name);
    if (!s)
        return HU_ERR_NOT_FOUND;
    size_t ignore = 0;
    return hu_skillforge_load_instructions(alloc, s, out_instructions, &ignore);
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

typedef struct {
    size_t idx;
    int score;
} hu_skill_rank_t;

static int hu_skill_rank_cmp(const void *a, const void *b) {
    const hu_skill_rank_t *x = (const hu_skill_rank_t *)a;
    const hu_skill_rank_t *y = (const hu_skill_rank_t *)b;
    if (x->score != y->score)
        return y->score - x->score;
    return 0; /* name tie-break filled by stable pre-sort */
}

static void lower_inplace(char *s) {
    if (!s)
        return;
    for (; *s; s++)
        *s = (char)tolower((unsigned char)*s);
}

int hu_skillforge_skill_keyword_hits(const hu_skill_t *skill, const char *user_msg,
                                     size_t user_msg_len) {
    if (!skill || !user_msg || user_msg_len == 0)
        return 0;

    char ubuf[4096];
    size_t ulen = user_msg_len < sizeof(ubuf) - 1 ? user_msg_len : sizeof(ubuf) - 1;
    memcpy(ubuf, user_msg, ulen);
    ubuf[ulen] = '\0';
    lower_inplace(ubuf);

    const char *nm = skill->name ? skill->name : "";
    const char *ds = skill->description ? skill->description : "";
    char blob[2048];
    int bn = snprintf(blob, sizeof(blob), "%s %s", nm, ds);
    if (bn < 0)
        bn = 0;
    if ((size_t)bn >= sizeof(blob))
        blob[sizeof(blob) - 1] = '\0';
    else
        blob[(size_t)bn] = '\0';
    lower_inplace(blob);

    int sc = 0;
    const char *p = ubuf;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p))
            p++;
        if (!*p)
            break;
        const char *w = p;
        while (*p && isalnum((unsigned char)*p))
            p++;
        size_t wlen = (size_t)(p - w);
        if (wlen < 2 || wlen > 48)
            continue;
        char tmp[49];
        memcpy(tmp, w, wlen);
        tmp[wlen] = '\0';
        if (strstr(blob, tmp))
            sc++;
    }
    return sc;
}

hu_error_t hu_skillforge_build_prompt_catalog(hu_allocator_t *alloc, hu_skillforge_t *sf,
                                              const char *user_msg, size_t user_msg_len, char **out,
                                              size_t *out_len) {
    if (!alloc || !sf || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    size_t ena[256];
    size_t en = 0;
    for (size_t i = 0; i < sf->skills_len && en < 256; i++) {
        if (!sf->skills[i].enabled)
            continue;
        ena[en++] = i;
    }
    if (en == 0)
        return HU_OK;

    const char *mode = getenv("HUMAN_SKILLS_CONTEXT");
    int use_top_k =
        mode && strcmp(mode, "top_k") == 0 && user_msg && user_msg_len > 0 && user_msg_len < 65536;

    long k_long = 12;
    const char *ks = getenv("HUMAN_SKILLS_TOP_K");
    if (ks && ks[0]) {
        char *end = NULL;
        long v = strtol(ks, &end, 10);
        if (end != ks && v > 0 && v <= 64)
            k_long = v;
    }
    size_t top_k = (size_t)k_long;
    if (top_k < 1)
        top_k = 1;

    long max_b_long = 8192;
    const char *bs = getenv("HUMAN_SKILLS_CONTEXT_MAX_BYTES");
    if (bs && bs[0]) {
        char *end = NULL;
        long v = strtol(bs, &end, 10);
        if (end != bs && v >= 512 && v <= 65536)
            max_b_long = v;
    }
    size_t max_bytes = (size_t)max_b_long;

    hu_skill_rank_t ranks[256];
    size_t rn = 0;
    if (use_top_k && en > top_k) {
        char ubuf[4096];
        size_t ulen = user_msg_len < sizeof(ubuf) - 1 ? user_msg_len : sizeof(ubuf) - 1;
        memcpy(ubuf, user_msg, ulen);
        ubuf[ulen] = '\0';
        lower_inplace(ubuf);
        for (size_t e = 0; e < en; e++) {
            size_t i = ena[e];
            const char *nm = sf->skills[i].name ? sf->skills[i].name : "";
            const char *ds = sf->skills[i].description ? sf->skills[i].description : "";
            char blob[2048];
            int bn = snprintf(blob, sizeof(blob), "%s %s", nm, ds);
            if (bn < 0)
                bn = 0;
            if ((size_t)bn >= sizeof(blob))
                blob[sizeof(blob) - 1] = '\0';
            lower_inplace(blob);
            int sc = 0;
            const char *p = ubuf;
            while (*p) {
                while (*p && !isalnum((unsigned char)*p))
                    p++;
                if (!*p)
                    break;
                const char *w = p;
                while (*p && isalnum((unsigned char)*p))
                    p++;
                size_t wlen = (size_t)(p - w);
                if (wlen < 2 || wlen > 48)
                    continue;
                char tmp[49];
                memcpy(tmp, w, wlen);
                tmp[wlen] = '\0';
                if (strstr(blob, tmp))
                    sc++;
            }
            ranks[rn].idx = i;
            ranks[rn].score = sc;
            rn++;
        }
        qsort(ranks, rn, sizeof(ranks[0]), hu_skill_rank_cmp);
        /* Stable tie-break: sort by name among equal scores (insertion on ties) */
        for (size_t a = 1; a < rn; a++) {
            size_t b = a;
            while (b > 0 && ranks[b].score == ranks[b - 1].score) {
                const char *na = sf->skills[ranks[b].idx].name ? sf->skills[ranks[b].idx].name : "";
                const char *nb =
                    sf->skills[ranks[b - 1].idx].name ? sf->skills[ranks[b - 1].idx].name : "";
                if (strcmp(na, nb) >= 0)
                    break;
                hu_skill_rank_t t = ranks[b];
                ranks[b] = ranks[b - 1];
                ranks[b - 1] = t;
                b--;
            }
        }
    } else {
        for (size_t e = 0; e < en; e++) {
            ranks[e].idx = ena[e];
            ranks[e].score = 0;
        }
        rn = en;
        /* Alphabetical by name for non-top_k large lists consistency */
        for (size_t a = 1; a < rn; a++) {
            size_t b = a;
            while (b > 0) {
                const char *na = sf->skills[ranks[b].idx].name ? sf->skills[ranks[b].idx].name : "";
                const char *nb =
                    sf->skills[ranks[b - 1].idx].name ? sf->skills[ranks[b - 1].idx].name : "";
                if (strcmp(na, nb) >= 0)
                    break;
                hu_skill_rank_t t = ranks[b];
                ranks[b] = ranks[b - 1];
                ranks[b - 1] = t;
                b--;
            }
        }
    }

    size_t pick = use_top_k && en > top_k ? top_k : en;
    size_t total = 0;
    for (size_t p = 0; p < pick; p++) {
        size_t i = ranks[p].idx;
        total += 4 + strlen(sf->skills[i].name ? sf->skills[i].name : "") + 3 +
                 strlen(sf->skills[i].description ? sf->skills[i].description : "") + 1;
    }
    size_t omitted = en > pick ? en - pick : 0;
    if (omitted > 0)
        total += 80 + (omitted > 99 ? 3 : omitted > 9 ? 2 : 1);

    if (total > max_bytes) {
        while (pick > 1 && total > max_bytes) {
            size_t i = ranks[pick - 1].idx;
            size_t line = 4 + strlen(sf->skills[i].name ? sf->skills[i].name : "") + 3 +
                          strlen(sf->skills[i].description ? sf->skills[i].description : "") + 1;
            total -= line;
            pick--;
        }
        omitted = en > pick ? en - pick : 0;
    }

    /* +256 slop: per-line snprintf can exceed the strlen sum slightly */
    char *buf = (char *)alloc->alloc(alloc->ctx, total + 256);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t cap = total + 256;
    size_t pos = 0;
    for (size_t p = 0; p < pick; p++) {
        size_t i = ranks[p].idx;
        pos = hu_buf_appendf(buf, cap, pos, "- %s: %s\n",
                             sf->skills[i].name ? sf->skills[i].name : "",
                             sf->skills[i].description ? sf->skills[i].description : "");
    }
    if (omitted > 0) {
        pos = hu_buf_appendf(buf, cap, pos,
                             "\n(%zu more skills installed; use skill_run with a skill name for full "
                             "instructions.)\n",
                             omitted);
    }
    buf[pos] = '\0';
    *out = buf;
    *out_len = pos;
    return HU_OK;
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
                int n =
                    snprintf(path, sizeof(path), "%s/.human/skills/%.256s.skill.json", home, name);
                if (n > 0 && (size_t)n < sizeof(path))
                    remove(path);
            }
            return HU_OK;
        }
    }
    return HU_ERR_NOT_FOUND;
}
