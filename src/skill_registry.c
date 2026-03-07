/*
 * Skill registry — fetch, search, install, uninstall, update from remote registry.
 * Under SC_IS_TEST, all network and filesystem operations return mock/OK.
 */
#include "seaclaw/skill_registry.h"
#include "seaclaw/core/http.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static void entry_free(sc_allocator_t *a, sc_skill_registry_entry_t *e) {
    if (!a || !e)
        return;
    if (e->name)
        a->free(a->ctx, e->name, strlen(e->name) + 1);
    if (e->description)
        a->free(a->ctx, e->description, strlen(e->description) + 1);
    if (e->version)
        a->free(a->ctx, e->version, strlen(e->version) + 1);
    if (e->author)
        a->free(a->ctx, e->author, strlen(e->author) + 1);
    if (e->url)
        a->free(a->ctx, e->url, strlen(e->url) + 1);
    if (e->tags)
        a->free(a->ctx, e->tags, strlen(e->tags) + 1);
    memset(e, 0, sizeof(*e));
}

#ifdef SC_IS_TEST
/* Mock implementations — no network, no filesystem */
sc_error_t sc_skill_registry_search(sc_allocator_t *alloc, const char *query,
                                    sc_skill_registry_entry_t **out_entries, size_t *out_count) {
    (void)query;
    if (!alloc || !out_entries || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_entries = NULL;
    *out_count = 0;

    sc_skill_registry_entry_t *arr = (sc_skill_registry_entry_t *)alloc->alloc(
        alloc->ctx, 2 * sizeof(sc_skill_registry_entry_t));
    if (!arr)
        return SC_ERR_OUT_OF_MEMORY;
    memset(arr, 0, 2 * sizeof(sc_skill_registry_entry_t));

    arr[0].name = sc_strdup(alloc, "code-review");
    arr[0].description = sc_strdup(alloc, "Automated code review");
    arr[0].version = sc_strdup(alloc, "1.0.0");
    arr[0].author = sc_strdup(alloc, "seaclaw");
    arr[0].url =
        sc_strdup(alloc, "https://github.com/seaclaw/skill-registry/tree/main/skills/code-review");
    arr[0].tags = sc_strdup(alloc, "development,review");
    arr[1].name = sc_strdup(alloc, "email-digest");
    arr[1].description = sc_strdup(alloc, "Daily email digest");
    arr[1].version = sc_strdup(alloc, "1.0.0");
    arr[1].author = sc_strdup(alloc, "seaclaw");
    arr[1].url =
        sc_strdup(alloc, "https://github.com/seaclaw/skill-registry/tree/main/skills/email-digest");
    arr[1].tags = sc_strdup(alloc, "email,productivity");

    *out_entries = arr;
    *out_count = 2;
    return SC_OK;
}

void sc_skill_registry_entries_free(sc_allocator_t *alloc, sc_skill_registry_entry_t *entries,
                                    size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++)
        entry_free(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_skill_registry_entry_t));
}

/* install/uninstall/update/publish: reserved for future remote registry support.
   Currently no-ops — skills are loaded from disk via sc_skill_registry_search(). */

sc_error_t sc_skill_registry_install(sc_allocator_t *alloc, const char *name) {
    (void)alloc;
    (void)name;
    return SC_ERR_NOT_SUPPORTED;
}

sc_error_t sc_skill_registry_uninstall(const char *name) {
    (void)name;
    return SC_ERR_NOT_SUPPORTED;
}

sc_error_t sc_skill_registry_update(sc_allocator_t *alloc) {
    (void)alloc;
    return SC_ERR_NOT_SUPPORTED;
}

sc_error_t sc_skill_registry_publish(sc_allocator_t *alloc, const char *skill_dir) {
    if (!alloc || !skill_dir)
        return SC_ERR_INVALID_ARGUMENT;
    return SC_ERR_NOT_SUPPORTED;
}

size_t sc_skill_registry_get_installed_dir(char *out, size_t out_len) {
    if (!out || out_len == 0)
        return 0;
    const char *home = getenv("HOME");
    if (!home) {
        out[0] = '\0';
        return 0;
    }
    int n = snprintf(out, out_len, "%s/.seaclaw/skills", home);
    return (n > 0 && (size_t)n < out_len) ? (size_t)n : 0;
}

#else /* !SC_IS_TEST */

static bool matches_query(const char *q, const char *name, const char *desc, const char *tags) {
    if (!q || !q[0])
        return true;
    if (!name && !desc && !tags)
        return false;
    const char *fields[] = {name, desc, tags};
    for (int i = 0; i < 3; i++) {
        const char *v = fields[i];
        if (!v)
            continue;
        size_t ql = strlen(q);
        size_t vl = strlen(v);
        for (size_t j = 0; j + ql <= vl; j++) {
            if (strncasecmp(v + j, q, ql) == 0)
                return true;
        }
    }
    return false;
}

sc_error_t sc_skill_registry_search(sc_allocator_t *alloc, const char *query,
                                    sc_skill_registry_entry_t **out_entries, size_t *out_count) {
    if (!alloc || !out_entries || !out_count)
        return SC_ERR_INVALID_ARGUMENT;
    *out_entries = NULL;
    *out_count = 0;

    sc_http_response_t resp = {0};
    sc_error_t err = sc_http_get(alloc, SC_SKILL_REGISTRY_URL, NULL, &resp);
    if (err != SC_OK || !resp.body || resp.body_len == 0) {
        sc_http_response_free(alloc, &resp);
        return err != SC_OK ? err : SC_ERR_PROVIDER_RESPONSE;
    }

    sc_json_value_t *root = NULL;
    err = sc_json_parse(alloc, resp.body, resp.body_len, &root);
    sc_http_response_free(alloc, &resp);
    if (err != SC_OK || !root || root->type != SC_JSON_ARRAY) {
        if (root)
            sc_json_free(alloc, root);
        return SC_ERR_PARSE;
    }

    size_t cap = 16;
    sc_skill_registry_entry_t *arr = (sc_skill_registry_entry_t *)alloc->alloc(
        alloc->ctx, cap * sizeof(sc_skill_registry_entry_t));
    if (!arr) {
        sc_json_free(alloc, root);
        return SC_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, cap * sizeof(sc_skill_registry_entry_t));

    size_t count = 0;
    for (size_t i = 0; i < root->data.array.len && count < 256; i++) {
        sc_json_value_t *obj = root->data.array.items[i];
        if (!obj || obj->type != SC_JSON_OBJECT)
            continue;

        const char *name = sc_json_get_string(obj, "name");
        const char *desc = sc_json_get_string(obj, "description");
        const char *tags_str = NULL;
        sc_json_value_t *tags_val = sc_json_object_get(obj, "tags");
        if (tags_val) {
            if (tags_val->type == SC_JSON_STRING && tags_val->data.string.ptr)
                tags_str = tags_val->data.string.ptr;
            else if (tags_val->type == SC_JSON_ARRAY && tags_val->data.array.len > 0) {
                char tags_buf[256];
                size_t pos = 0;
                for (size_t j = 0; j < tags_val->data.array.len && pos < sizeof(tags_buf) - 2;
                     j++) {
                    sc_json_value_t *t = tags_val->data.array.items[j];
                    if (t && t->type == SC_JSON_STRING && t->data.string.ptr) {
                        if (j > 0) {
                            tags_buf[pos++] = ',';
                            tags_buf[pos++] = ' ';
                        }
                        size_t len = t->data.string.len;
                        if (pos + len >= sizeof(tags_buf))
                            len = sizeof(tags_buf) - pos - 1;
                        memcpy(tags_buf + pos, t->data.string.ptr, len);
                        pos += len;
                    }
                }
                tags_buf[pos] = '\0';
                if (pos > 0)
                    tags_str = tags_buf; /* only for match, not stored */
            }
        }
        if (!name)
            continue;
        if (query && query[0] && !matches_query(query, name, desc, tags_str))
            continue;

        if (count >= cap) {
            size_t new_cap = cap * 2;
            sc_skill_registry_entry_t *n = (sc_skill_registry_entry_t *)alloc->realloc(
                alloc->ctx, arr, cap * sizeof(sc_skill_registry_entry_t),
                new_cap * sizeof(sc_skill_registry_entry_t));
            if (!n) {
                for (size_t k = 0; k < count; k++)
                    entry_free(alloc, &arr[k]);
                alloc->free(alloc->ctx, arr, cap * sizeof(sc_skill_registry_entry_t));
                sc_json_free(alloc, root);
                return SC_ERR_OUT_OF_MEMORY;
            }
            arr = n;
            cap = new_cap;
            memset(arr + count, 0, (cap - count) * sizeof(sc_skill_registry_entry_t));
        }

        sc_skill_registry_entry_t *e = &arr[count];
        e->name = name ? sc_strdup(alloc, name) : NULL;
        e->description = desc ? sc_strdup(alloc, desc) : sc_strdup(alloc, "");
        {
            const char *v = sc_json_get_string(obj, "version");
            e->version = sc_strdup(alloc, v && v[0] ? v : "1.0.0");
        }
        {
            const char *a = sc_json_get_string(obj, "author");
            e->author = sc_strdup(alloc, a && a[0] ? a : "");
        }
        {
            const char *u = sc_json_get_string(obj, "url");
            e->url = sc_strdup(alloc, u && u[0] ? u : "");
        }
        e->tags = tags_str ? sc_strdup(alloc, tags_str) : NULL;
        if (!e->name) {
            entry_free(alloc, e);
            continue;
        }
        count++;
    }

    sc_json_free(alloc, root);
    *out_entries = arr;
    *out_count = count;
    return SC_OK;
}

void sc_skill_registry_entries_free(sc_allocator_t *alloc, sc_skill_registry_entry_t *entries,
                                    size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++)
        entry_free(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(sc_skill_registry_entry_t));
}

/* Convert GitHub tree URL to raw URL base. */
static void url_tree_to_raw(const char *tree_url, char *raw_base, size_t raw_len) {
    const char *p = strstr(tree_url, "github.com/");
    if (!p || raw_len < 32) {
        if (raw_len > 0)
            raw_base[0] = '\0';
        return;
    }
    p += 11;
    const char *tree = strstr(p, "/tree/");
    if (!tree) {
        if (raw_len > 0)
            raw_base[0] = '\0';
        return;
    }
    size_t org_repo = (size_t)(tree - p);
    const char *branch_start = tree + 6;
    const char *slash = strchr(branch_start, '/');
    size_t branch_len = slash ? (size_t)(slash - branch_start) : strlen(branch_start);
    const char *path = slash ? slash + 1 : "";
    int n = snprintf(raw_base, raw_len, "https://raw.githubusercontent.com/%.*s/%.*s/%s",
                     (int)org_repo, p, (int)branch_len, branch_start, path);
    if (n < 0 || (size_t)n >= raw_len)
        raw_base[0] = '\0';
}

sc_error_t sc_skill_registry_install(sc_allocator_t *alloc, const char *name) {
    if (!alloc || !name || !name[0])
        return SC_ERR_INVALID_ARGUMENT;

    sc_skill_registry_entry_t *entries = NULL;
    size_t count = 0;
    sc_error_t err = sc_skill_registry_search(alloc, NULL, &entries, &count);
    if (err != SC_OK)
        return err;

    const char *url = NULL;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].name && strcmp(entries[i].name, name) == 0) {
            url = entries[i].url;
            break;
        }
    }

    if (!url || !url[0]) {
        sc_skill_registry_entries_free(alloc, entries, count);
        return SC_ERR_NOT_FOUND;
    }

    char raw_base[512];
    url_tree_to_raw(url, raw_base, sizeof(raw_base));
    if (!raw_base[0]) {
        sc_skill_registry_entries_free(alloc, entries, count);
        return SC_ERR_INVALID_ARGUMENT;
    }

    /* Ensure path ends with / */
    size_t lb = strlen(raw_base);
    if (lb > 0 && raw_base[lb - 1] != '/') {
        if (lb + 1 < sizeof(raw_base)) {
            raw_base[lb] = '/';
            raw_base[lb + 1] = '\0';
        }
    }

    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        sc_skill_registry_entries_free(alloc, entries, count);
        return SC_ERR_INVALID_ARGUMENT;
    }

    char skill_path[512];
    int n =
        snprintf(skill_path, sizeof(skill_path), "%s/.seaclaw/skills/%s.skill.json", home, name);
    if (n <= 0 || (size_t)n >= sizeof(skill_path)) {
        sc_skill_registry_entries_free(alloc, entries, count);
        return SC_ERR_INVALID_ARGUMENT;
    }

    char skill_json_url[640];
    n = snprintf(skill_json_url, sizeof(skill_json_url), "%s%s.skill.json", raw_base, name);
    if (n <= 0 || (size_t)n >= sizeof(skill_json_url)) {
        sc_skill_registry_entries_free(alloc, entries, count);
        return SC_ERR_INVALID_ARGUMENT;
    }

#ifndef _WIN32
    char dir_path[512];
    n = snprintf(dir_path, sizeof(dir_path), "%s/.seaclaw/skills", home);
    if (n > 0 && (size_t)n < sizeof(dir_path))
        mkdir(dir_path, 0755);
#endif

    sc_http_response_t resp = {0};
    err = sc_http_get(alloc, skill_json_url, NULL, &resp);
    if (err != SC_OK || !resp.body) {
        sc_http_response_free(alloc, &resp);
        sc_skill_registry_entries_free(alloc, entries, count);
        return err != SC_OK ? err : SC_ERR_PROVIDER_RESPONSE;
    }

    FILE *f = fopen(skill_path, "wb");
    if (!f) {
        sc_http_response_free(alloc, &resp);
        sc_skill_registry_entries_free(alloc, entries, count);
        return SC_ERR_IO;
    }
    size_t written = fwrite(resp.body, 1, resp.body_len, f);
    fclose(f);
    sc_http_response_free(alloc, &resp);
    if (written != resp.body_len) {
        remove(skill_path);
        sc_skill_registry_entries_free(alloc, entries, count);
        return SC_ERR_IO;
    }

    /* Optionally fetch SKILL.md */
    char skill_md_url[640];
    n = snprintf(skill_md_url, sizeof(skill_md_url), "%sSKILL.md", raw_base);
    if (n > 0 && (size_t)n < sizeof(skill_md_url)) {
        sc_http_response_t md_resp = {0};
        if (sc_http_get(alloc, skill_md_url, NULL, &md_resp) == SC_OK && md_resp.body &&
            md_resp.body_len > 0) {
            char md_path[512];
            n = snprintf(md_path, sizeof(md_path), "%s/.seaclaw/skills/%s/SKILL.md", home, name);
            if (n > 0 && (size_t)n < sizeof(md_path)) {
#ifndef _WIN32
                char skill_dir[512];
                n = snprintf(skill_dir, sizeof(skill_dir), "%s/.seaclaw/skills/%s", home, name);
                if (n > 0 && (size_t)n < sizeof(skill_dir))
                    mkdir(skill_dir, 0755);
#endif
                FILE *mf = fopen(md_path, "wb");
                if (mf) {
                    fwrite(md_resp.body, 1, md_resp.body_len, mf);
                    fclose(mf);
                }
            }
            sc_http_response_free(alloc, &md_resp);
        }
    }

    sc_skill_registry_entries_free(alloc, entries, count);
    return SC_OK;
}

sc_error_t sc_skill_registry_uninstall(const char *name) {
    if (!name || !name[0])
        return SC_ERR_INVALID_ARGUMENT;

    const char *home = getenv("HOME");
    if (!home || !home[0])
        return SC_ERR_INVALID_ARGUMENT;

    char path[512];
    int n = snprintf(path, sizeof(path), "%s/.seaclaw/skills/%.256s.skill.json", home, name);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return SC_ERR_INVALID_ARGUMENT;

    if (remove(path) != 0)
        return SC_ERR_NOT_FOUND;

    return SC_OK;
}

sc_error_t sc_skill_registry_update(sc_allocator_t *alloc) {
    if (!alloc)
        return SC_ERR_INVALID_ARGUMENT;

    char dir_path[512];
    size_t dlen = sc_skill_registry_get_installed_dir(dir_path, sizeof(dir_path));
    if (dlen == 0)
        return SC_OK;

#if !defined(_WIN32)
    {
        DIR *d = opendir(dir_path);
        if (!d)
            return SC_OK;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            if (e->d_name[0] == '.')
                continue;
            size_t nlen = strlen(e->d_name);
            if (nlen < 12 || strcmp(e->d_name + nlen - 11, ".skill.json") != 0)
                continue;
            char skill_name[260];
            if (nlen <= 11)
                continue;
            size_t name_len = nlen - 11;
            if (name_len >= sizeof(skill_name))
                continue;
            memcpy(skill_name, e->d_name, name_len);
            skill_name[name_len] = '\0';

            sc_skill_registry_install(alloc, skill_name);
        }
        closedir(d);
    }
#endif
    return SC_OK;
}

sc_error_t sc_skill_registry_publish(sc_allocator_t *alloc, const char *skill_dir) {
    if (!alloc || !skill_dir)
        return SC_ERR_INVALID_ARGUMENT;

    char json_path[512];
    char md_path[512];
    int n = snprintf(json_path, sizeof(json_path), "%s/.skill.json", skill_dir);
    if (n < 0 || (size_t)n >= sizeof(json_path))
        return SC_ERR_INVALID_ARGUMENT;
    n = snprintf(md_path, sizeof(md_path), "%s/SKILL.md", skill_dir);
    if (n < 0 || (size_t)n >= sizeof(md_path))
        return SC_ERR_INVALID_ARGUMENT;

    bool has_json = false;
    bool has_md = false;
    {
        FILE *fp = fopen(json_path, "rb");
        if (fp) {
            has_json = true;
            fclose(fp);
        }
    }
    {
        FILE *fp = fopen(md_path, "rb");
        if (fp) {
            has_md = true;
            fclose(fp);
        }
    }
    if (!has_json && !has_md)
        return SC_ERR_NOT_FOUND;

    if (!has_json) {
        printf("To publish, submit a PR to https://github.com/seaclaw/skill-registry with your "
               "skill manifest.\n");
        return SC_OK;
    }

    char buf[4096];
    FILE *f = fopen(json_path, "rb");
    if (!f)
        return SC_ERR_IO;
    size_t read_len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (read_len == 0 || read_len >= sizeof(buf))
        return SC_ERR_IO;
    buf[read_len] = '\0';

    sc_json_value_t *parsed = NULL;
    sc_error_t err = sc_json_parse(alloc, buf, read_len, &parsed);
    if (err != SC_OK || !parsed || parsed->type != SC_JSON_OBJECT) {
        if (parsed)
            sc_json_free(alloc, parsed);
        return err != SC_OK ? err : SC_ERR_PARSE;
    }

    const char *name = sc_json_get_string(parsed, "name");
    const char *desc = sc_json_get_string(parsed, "description");
    if (!name || !name[0] || !desc) {
        sc_json_free(alloc, parsed);
        return SC_ERR_PARSE;
    }
    sc_json_free(alloc, parsed);

    printf("To publish, submit a PR to https://github.com/seaclaw/skill-registry with your skill "
           "manifest.\n");
    return SC_OK;
}

size_t sc_skill_registry_get_installed_dir(char *out, size_t out_len) {
    if (!out || out_len == 0)
        return 0;
    const char *home = getenv("HOME");
    if (!home) {
        out[0] = '\0';
        return 0;
    }
    int n = snprintf(out, out_len, "%s/.seaclaw/skills", home);
    return (n > 0 && (size_t)n < out_len) ? (size_t)n : 0;
}

#endif /* !SC_IS_TEST */
