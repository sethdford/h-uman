/*
 * Skill registry — fetch, search, install, uninstall, update from remote registry.
 * Under HU_IS_TEST, all network and filesystem operations return mock/OK.
 */
#include "human/skill_registry.h"
#include "human/core/http.h"
#include "human/core/io_secure.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/crypto.h"
#include "human/update.h"
#include <ctype.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static void entry_free(hu_allocator_t *a, hu_skill_registry_entry_t *e) {
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
    if (e->sha256)
        a->free(a->ctx, e->sha256, strlen(e->sha256) + 1);
    if (e->tags)
        a->free(a->ctx, e->tags, strlen(e->tags) + 1);
    memset(e, 0, sizeof(*e));
}

/* Registry search tag handling; also linked by test_skills.c for array/string coverage. */
void hu_skill_registry_resolve_tags_string(hu_json_value_t *tags_val, char *tags_buf, size_t tags_buf_len,
                                         const char **out_tags_str) {
    *out_tags_str = NULL;
    if (!tags_val || tags_buf_len == 0)
        return;
    if (tags_val->type == HU_JSON_STRING && tags_val->data.string.ptr)
        *out_tags_str = tags_val->data.string.ptr;
    else if (tags_val->type == HU_JSON_ARRAY && tags_val->data.array.len > 0) {
        size_t pos = 0;
        for (size_t j = 0; j < tags_val->data.array.len && pos < tags_buf_len - 2; j++) {
            hu_json_value_t *t = tags_val->data.array.items[j];
            if (t && t->type == HU_JSON_STRING && t->data.string.ptr) {
                if (j > 0) {
                    tags_buf[pos++] = ',';
                    tags_buf[pos++] = ' ';
                }
                size_t len = t->data.string.len;
                if (pos + len >= tags_buf_len)
                    len = tags_buf_len - pos - 1;
                memcpy(tags_buf + pos, t->data.string.ptr, len);
                pos += len;
            }
        }
        tags_buf[pos] = '\0';
        if (pos > 0)
            *out_tags_str = tags_buf;
    }
}

#ifdef HU_IS_TEST
/* Mock implementations — no network, no filesystem */
hu_error_t hu_skill_registry_search(hu_allocator_t *alloc, const char *registry_url,
                                    const char *query, hu_skill_registry_entry_t **out_entries,
                                    size_t *out_count) {
    (void)registry_url;
    (void)query;
    if (!alloc || !out_entries || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_entries = NULL;
    *out_count = 0;

    hu_skill_registry_entry_t *arr = (hu_skill_registry_entry_t *)alloc->alloc(
        alloc->ctx, 2 * sizeof(hu_skill_registry_entry_t));
    if (!arr)
        return HU_ERR_OUT_OF_MEMORY;
    memset(arr, 0, 2 * sizeof(hu_skill_registry_entry_t));

    arr[0].name = hu_strdup(alloc, "code-review");
    arr[0].description = hu_strdup(alloc, "Automated code review");
    arr[0].version = hu_strdup(alloc, "1.0.0");
    arr[0].author = hu_strdup(alloc, "human");
    arr[0].url =
        hu_strdup(alloc, "https://github.com/human/skill-registry/tree/main/skills/code-review");
    arr[0].tags = hu_strdup(alloc, "development,review");
    arr[1].name = hu_strdup(alloc, "code-formatter");
    arr[1].description = hu_strdup(alloc, "Format and lint code");
    arr[1].version = hu_strdup(alloc, "1.0.0");
    arr[1].author = hu_strdup(alloc, "human");
    arr[1].url =
        hu_strdup(alloc, "https://github.com/human/skill-registry/tree/main/skills/code-formatter");
    arr[1].tags = hu_strdup(alloc, "development,formatting");

    *out_entries = arr;
    *out_count = 2;
    return HU_OK;
}

void hu_skill_registry_entries_free(hu_allocator_t *alloc, hu_skill_registry_entry_t *entries,
                                    size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++)
        entry_free(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(hu_skill_registry_entry_t));
}

/* install/uninstall/update/publish: local filesystem only.
   Under HU_IS_TEST, validate input then return HU_OK with no side effects. */

hu_error_t hu_skill_registry_install(hu_allocator_t *alloc, const char *source_path) {
    if (!alloc || !source_path || !source_path[0])
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

hu_error_t hu_skill_registry_install_by_name(hu_allocator_t *alloc, const char *registry_url,
                                             const char *name) {
    (void)registry_url;
    if (!alloc || !name || !name[0])
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

hu_error_t hu_skill_registry_verify(hu_allocator_t *alloc, const char *registry_url,
                                    const char *name) {
    (void)registry_url;
    if (!alloc || !name || !name[0])
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

hu_error_t hu_skill_registry_upgrade(hu_allocator_t *alloc, const char *registry_url,
                                     const char *name) {
    (void)registry_url;
    if (!alloc || !name || !name[0])
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

hu_error_t hu_skill_registry_uninstall(const char *name) {
    if (!name || !name[0])
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

hu_error_t hu_skill_registry_update(hu_allocator_t *alloc, const char *source_path) {
    if (!alloc || !source_path || !source_path[0])
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

hu_error_t hu_skill_registry_publish(hu_allocator_t *alloc, const char *skill_dir) {
    if (!alloc || !skill_dir)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
}

size_t hu_skill_registry_get_installed_dir(char *out, size_t out_len) {
    if (!out || out_len == 0)
        return 0;
    const char *home = getenv("HOME");
    if (!home) {
        out[0] = '\0';
        return 0;
    }
    int n = snprintf(out, out_len, "%s/.human/skills", home);
    return (n > 0 && (size_t)n < out_len) ? (size_t)n : 0;
}

#else /* !HU_IS_TEST */

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

hu_error_t hu_skill_registry_search(hu_allocator_t *alloc, const char *registry_url,
                                    const char *query, hu_skill_registry_entry_t **out_entries,
                                    size_t *out_count) {
    if (!alloc || !out_entries || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_entries = NULL;
    *out_count = 0;

    const char *url = (registry_url && registry_url[0]) ? registry_url : HU_SKILL_REGISTRY_URL;
    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url, NULL, &resp);
    if (err != HU_OK || !resp.body || resp.body_len == 0) {
        hu_http_response_free(alloc, &resp);
        return err != HU_OK ? err : HU_ERR_PROVIDER_RESPONSE;
    }

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, resp.body, resp.body_len, &root);
    hu_http_response_free(alloc, &resp);
    if (err != HU_OK || !root || root->type != HU_JSON_ARRAY) {
        if (root)
            hu_json_free(alloc, root);
        return HU_ERR_PARSE;
    }

    size_t cap = 16;
    hu_skill_registry_entry_t *arr = (hu_skill_registry_entry_t *)alloc->alloc(
        alloc->ctx, cap * sizeof(hu_skill_registry_entry_t));
    if (!arr) {
        hu_json_free(alloc, root);
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(arr, 0, cap * sizeof(hu_skill_registry_entry_t));

    char tags_buf[256];
    size_t count = 0;
    for (size_t i = 0; i < root->data.array.len && count < 256; i++) {
        hu_json_value_t *obj = root->data.array.items[i];
        if (!obj || obj->type != HU_JSON_OBJECT)
            continue;

        const char *name = hu_json_get_string(obj, "name");
        const char *desc = hu_json_get_string(obj, "description");
        const char *tags_str = NULL;
        hu_json_value_t *tags_val = hu_json_object_get(obj, "tags");
        hu_skill_registry_resolve_tags_string(tags_val, tags_buf, sizeof(tags_buf), &tags_str);
        if (!name)
            continue;
        if (query && query[0] && !matches_query(query, name, desc, tags_str))
            continue;

        if (count >= cap) {
            size_t new_cap = cap * 2;
            hu_skill_registry_entry_t *n = (hu_skill_registry_entry_t *)alloc->realloc(
                alloc->ctx, arr, cap * sizeof(hu_skill_registry_entry_t),
                new_cap * sizeof(hu_skill_registry_entry_t));
            if (!n) {
                for (size_t k = 0; k < count; k++)
                    entry_free(alloc, &arr[k]);
                alloc->free(alloc->ctx, arr, cap * sizeof(hu_skill_registry_entry_t));
                hu_json_free(alloc, root);
                return HU_ERR_OUT_OF_MEMORY;
            }
            arr = n;
            cap = new_cap;
            memset(arr + count, 0, (cap - count) * sizeof(hu_skill_registry_entry_t));
        }

        hu_skill_registry_entry_t *e = &arr[count];
        e->name = name ? hu_strdup(alloc, name) : NULL;
        e->description = desc ? hu_strdup(alloc, desc) : hu_strdup(alloc, "");
        {
            const char *v = hu_json_get_string(obj, "version");
            e->version = hu_strdup(alloc, v && v[0] ? v : "1.0.0");
        }
        {
            const char *a = hu_json_get_string(obj, "author");
            e->author = hu_strdup(alloc, a && a[0] ? a : "");
        }
        {
            const char *u = hu_json_get_string(obj, "url");
            e->url = hu_strdup(alloc, u && u[0] ? u : "");
        }
        e->tags = tags_str ? hu_strdup(alloc, tags_str) : NULL;
        if (!e->name) {
            entry_free(alloc, e);
            continue;
        }
        count++;
    }

    hu_json_free(alloc, root);
    *out_entries = arr;
    *out_count = count;
    return HU_OK;
}

void hu_skill_registry_entries_free(hu_allocator_t *alloc, hu_skill_registry_entry_t *entries,
                                    size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++)
        entry_free(alloc, &entries[i]);
    alloc->free(alloc->ctx, entries, count * sizeof(hu_skill_registry_entry_t));
}

#if defined(HU_GATEWAY_POSIX) && (!defined(HU_IS_TEST) || !HU_IS_TEST)

/* Copy src_file to dst_file. Returns HU_OK on success. */
static hu_error_t copy_file(const char *src_file, const char *dst_file) {
    FILE *src = fopen(src_file, "rb");
    if (!src)
        return HU_ERR_IO;
    /* Copies a skill file into the user's skill directory. Skills
     * are executable, but they're already user-owned trusted code
     * (skill_trust gate ran earlier); 0644 keeps the existing
     * permission semantics and adds the traversal guard. */
    FILE *dst = NULL;
    if (hu_io_secure_open(dst_file, HU_IO_PERM_USER, "wb", &dst) != HU_OK || !dst) {
        fclose(src);
        return HU_ERR_IO;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            fclose(src);
            fclose(dst);
            unlink(dst_file);
            return HU_ERR_IO;
        }
    }
    fclose(src);
    fclose(dst);
    return HU_OK;
}

/* Find manifest path in dir: .skill.json, manifest.json, or first *.skill.json. */
static bool find_manifest_in_dir(const char *dir, char *out_path, size_t out_len) {
    char path[512];
    int n = snprintf(path, sizeof(path), "%s/.skill.json", dir);
    if (n > 0 && (size_t)n < sizeof(path)) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out_path, out_len, "%s", path);
            return true;
        }
    }
    n = snprintf(path, sizeof(path), "%s/manifest.json", dir);
    if (n > 0 && (size_t)n < sizeof(path)) {
        struct stat st;
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out_path, out_len, "%s", path);
            return true;
        }
    }
#ifndef _WIN32
    {
        DIR *d = opendir(dir);
        if (!d)
            return false;
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            size_t len = strlen(e->d_name);
            if (len < 12 || strcmp(e->d_name + len - 11, ".skill.json") != 0)
                continue;
            n = snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
            closedir(d);
            if (n > 0 && (size_t)n < sizeof(path)) {
                struct stat st;
                if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
                    snprintf(out_path, out_len, "%s", path);
                    return true;
                }
            }
            return false;
        }
        closedir(d);
    }
#endif
    return false;
}

/* Recursively remove directory contents and the directory. */
static hu_error_t remove_dir_recursive(const char *path) {
#ifndef _WIN32
    DIR *d = opendir(path);
    if (!d)
        return (errno == ENOENT) ? HU_OK : HU_ERR_IO;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.' && (e->d_name[1] == '\0' ||
                                    (e->d_name[1] == '.' && e->d_name[2] == '\0')))
            continue;
        char sub[512];
        int n = snprintf(sub, sizeof(sub), "%s/%s", path, e->d_name);
        if (n <= 0 || (size_t)n >= sizeof(sub)) {
            closedir(d);
            return HU_ERR_INVALID_ARGUMENT;
        }
        struct stat st;
        if (stat(sub, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            hu_error_t err = remove_dir_recursive(sub);
            if (err != HU_OK) {
                closedir(d);
                return err;
            }
        } else if (unlink(sub) != 0) {
            closedir(d);
            return HU_ERR_IO;
        }
    }
    closedir(d);
    if (rmdir(path) != 0)
        return HU_ERR_IO;
#endif
    return HU_OK;
}

/* https://github.com/org/repo/tree/branch/path → https://raw.githubusercontent.com/org/repo/branch/path/ */
static bool github_tree_url_to_raw_base(const char *tree_url, char *out, size_t out_len) {
    static const char prefix[] = "https://github.com/";
    if (!tree_url || strncmp(tree_url, prefix, sizeof(prefix) - 1U) != 0)
        return false;
    const char *path = tree_url + sizeof(prefix) - 1U;
    const char *tree = strstr(path, "/tree/");
    if (!tree)
        return false;
    size_t org_repo_len = (size_t)(tree - path);
    const char *after_tree = tree + 6;
    if (!after_tree[0])
        return false;
    int n = snprintf(out, out_len, "https://raw.githubusercontent.com/%.*s/%s/",
                     (int)org_repo_len, path, after_tree);
    return n > 0 && (size_t)n < out_len;
}

static hu_error_t write_bytes_to_file(const char *path, const void *data, size_t len) {
    /* Generic write helper used by skill install/publish — every
     * call site passes a path that came from the skill manifest, so
     * the traversal guard is genuinely defending us here. */
    FILE *f = NULL;
    if (hu_io_secure_open(path, HU_IO_PERM_USER, "wb", &f) != HU_OK || !f)
        return HU_ERR_IO;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        unlink(path);
        return HU_ERR_IO;
    }
    fclose(f);
    return HU_OK;
}

#endif /* HU_GATEWAY_POSIX && !HU_IS_TEST */

hu_error_t hu_skill_registry_install(hu_allocator_t *alloc, const char *source_path) {
    if (!alloc || !source_path || !source_path[0])
        return HU_ERR_INVALID_ARGUMENT;

#if !defined(HU_IS_TEST) || !HU_IS_TEST
#ifdef HU_GATEWAY_POSIX
    char manifest_path[512];
    if (!find_manifest_in_dir(source_path, manifest_path, sizeof(manifest_path)))
        return HU_ERR_NOT_FOUND;

    char buf[8192];
    FILE *f = fopen(manifest_path, "rb");
    if (!f)
        return HU_ERR_IO;
    size_t nr = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (nr == 0 || nr >= sizeof(buf) - 1)
        return HU_ERR_IO;
    buf[nr] = '\0';

    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, buf, nr, &parsed);
    if (err != HU_OK || !parsed || parsed->type != HU_JSON_OBJECT) {
        if (parsed)
            hu_json_free(alloc, parsed);
        return err != HU_OK ? err : HU_ERR_PARSE;
    }
    const char *name = hu_json_get_string(parsed, "name");
    if (!name || !name[0]) {
        hu_json_free(alloc, parsed);
        return HU_ERR_PARSE;
    }
    hu_json_free(alloc, parsed);

    const char *home = getenv("HOME");
    if (!home || !home[0])
        return HU_ERR_INVALID_ARGUMENT;

    char base_dir[512];
    int n = snprintf(base_dir, sizeof(base_dir), "%s/.human/skills", home);
    if (n <= 0 || (size_t)n >= sizeof(base_dir))
        return HU_ERR_INVALID_ARGUMENT;
    if (mkdir(base_dir, 0755) != 0 && errno != EEXIST)
        return HU_ERR_IO;

    char dest_dir[512];
    n = snprintf(dest_dir, sizeof(dest_dir), "%s/.human/skills/%.256s", home, name);
    if (n <= 0 || (size_t)n >= sizeof(dest_dir))
        return HU_ERR_INVALID_ARGUMENT;
    if (mkdir(dest_dir, 0755) != 0 && errno != EEXIST)
        return HU_ERR_IO;

    /* Copy manifest as manifest.json (skillforge discovers subdir/manifest.json) */
    char dest_manifest[512];
    n = snprintf(dest_manifest, sizeof(dest_manifest), "%s/manifest.json", dest_dir);
    if (n <= 0 || (size_t)n >= sizeof(dest_manifest))
        return HU_ERR_INVALID_ARGUMENT;
    err = copy_file(manifest_path, dest_manifest);
    if (err != HU_OK)
        return err;

    /* Copy SKILL.md if present */
    char src_md[512];
    n = snprintf(src_md, sizeof(src_md), "%s/SKILL.md", source_path);
    if (n > 0 && (size_t)n < sizeof(src_md)) {
        struct stat st;
        if (stat(src_md, &st) == 0 && S_ISREG(st.st_mode)) {
            char dst_md[512];
            n = snprintf(dst_md, sizeof(dst_md), "%s/SKILL.md", dest_dir);
            if (n > 0 && (size_t)n < sizeof(dst_md))
                copy_file(src_md, dst_md);
        }
    }

    /* Copy any other non-hidden files (e.g. supporting assets) */
#ifndef _WIN32
    {
        DIR *d = opendir(source_path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.')
                    continue;
                if (strcmp(e->d_name, "SKILL.md") == 0)
                    continue;
                size_t len = strlen(e->d_name);
                if (len >= 12 && strcmp(e->d_name + len - 11, ".skill.json") == 0)
                    continue;
                if (strcmp(e->d_name, "manifest.json") == 0)
                    continue;
                char src_file[512];
                n = snprintf(src_file, sizeof(src_file), "%s/%s", source_path, e->d_name);
                if (n <= 0 || (size_t)n >= sizeof(src_file))
                    continue;
                struct stat st;
                if (stat(src_file, &st) != 0 || !S_ISREG(st.st_mode))
                    continue;
                char dst_file[512];
                n = snprintf(dst_file, sizeof(dst_file), "%s/%s", dest_dir, e->d_name);
                if (n > 0 && (size_t)n < sizeof(dst_file))
                    copy_file(src_file, dst_file);
            }
            closedir(d);
        }
    }
#endif
    return HU_OK;
#else
    (void)alloc;
    (void)source_path;
    return HU_ERR_NOT_SUPPORTED;
#endif
#else
    (void)alloc;
    (void)source_path;
    return HU_OK;
#endif
}

hu_error_t hu_skill_registry_install_by_name(hu_allocator_t *alloc, const char *registry_url,
                                             const char *name) {
    if (!alloc || !name || !name[0])
        return HU_ERR_INVALID_ARGUMENT;

#if !defined(HU_IS_TEST) || !HU_IS_TEST
#ifdef HU_GATEWAY_POSIX
    hu_skill_registry_entry_t *entries = NULL;
    size_t count = 0;
    hu_error_t err = hu_skill_registry_search(alloc, registry_url, name, &entries, &count);
    if (err != HU_OK) {
        if (entries)
            hu_skill_registry_entries_free(alloc, entries, count);
        return err;
    }

    const hu_skill_registry_entry_t *match = NULL;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].name && strcmp(entries[i].name, name) == 0) {
            match = &entries[i];
            break;
        }
    }
    if (!match || !match->url || !match->url[0]) {
        hu_skill_registry_entries_free(alloc, entries, count);
        return HU_ERR_NOT_FOUND;
    }

    char raw_base[512];
    if (!github_tree_url_to_raw_base(match->url, raw_base, sizeof(raw_base))) {
        hu_skill_registry_entries_free(alloc, entries, count);
        return HU_ERR_PARSE;
    }
    hu_skill_registry_entries_free(alloc, entries, count);

    char manifest_url[768];
    int n = snprintf(manifest_url, sizeof(manifest_url), "%s%s.skill.json", raw_base, name);
    if (n <= 0 || (size_t)n >= sizeof(manifest_url))
        return HU_ERR_INVALID_ARGUMENT;

    hu_http_response_t mresp = {0};
    err = hu_http_get(alloc, manifest_url, NULL, &mresp);
    if (err != HU_OK || mresp.status_code != 200 || !mresp.body || mresp.body_len == 0) {
        hu_http_response_free(alloc, &mresp);
        return err != HU_OK ? err : HU_ERR_PROVIDER_RESPONSE;
    }

    hu_json_value_t *parsed = NULL;
    err = hu_json_parse(alloc, mresp.body, mresp.body_len, &parsed);
    if (err != HU_OK || !parsed || parsed->type != HU_JSON_OBJECT) {
        if (parsed)
            hu_json_free(alloc, parsed);
        hu_http_response_free(alloc, &mresp);
        return err != HU_OK ? err : HU_ERR_PARSE;
    }
    hu_json_free(alloc, parsed);

    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        hu_http_response_free(alloc, &mresp);
        return HU_ERR_INVALID_ARGUMENT;
    }

    char base_dir[512];
    n = snprintf(base_dir, sizeof(base_dir), "%s/.human/skills", home);
    if (n <= 0 || (size_t)n >= sizeof(base_dir)) {
        hu_http_response_free(alloc, &mresp);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (mkdir(base_dir, 0755) != 0 && errno != EEXIST) {
        hu_http_response_free(alloc, &mresp);
        return HU_ERR_IO;
    }

    char dest_dir[512];
    n = snprintf(dest_dir, sizeof(dest_dir), "%s/.human/skills/%.256s", home, name);
    if (n <= 0 || (size_t)n >= sizeof(dest_dir)) {
        hu_http_response_free(alloc, &mresp);
        return HU_ERR_INVALID_ARGUMENT;
    }
    if (mkdir(dest_dir, 0755) != 0 && errno != EEXIST) {
        hu_http_response_free(alloc, &mresp);
        return HU_ERR_IO;
    }

    char dest_manifest[640];
    n = snprintf(dest_manifest, sizeof(dest_manifest), "%s/manifest.json", dest_dir);
    if (n <= 0 || (size_t)n >= sizeof(dest_manifest)) {
        hu_http_response_free(alloc, &mresp);
        return HU_ERR_INVALID_ARGUMENT;
    }

    err = write_bytes_to_file(dest_manifest, mresp.body, mresp.body_len);
    hu_http_response_free(alloc, &mresp);
    if (err != HU_OK)
        return err;

    char md_url[768];
    n = snprintf(md_url, sizeof(md_url), "%sSKILL.md", raw_base);
    if (n > 0 && (size_t)n < sizeof(md_url)) {
        hu_http_response_t mdresp = {0};
        hu_error_t merr = hu_http_get(alloc, md_url, NULL, &mdresp);
        if (merr == HU_OK && mdresp.status_code == 200 && mdresp.body && mdresp.body_len > 0) {
            char dst_md[640];
            n = snprintf(dst_md, sizeof(dst_md), "%s/SKILL.md", dest_dir);
            if (n > 0 && (size_t)n < sizeof(dst_md))
                (void)write_bytes_to_file(dst_md, mdresp.body, mdresp.body_len);
        }
        hu_http_response_free(alloc, &mdresp);
    }

    return HU_OK;
#else
    (void)alloc;
    (void)registry_url;
    (void)name;
    return HU_ERR_NOT_SUPPORTED;
#endif
#else
    (void)alloc;
    (void)registry_url;
    (void)name;
    return HU_OK;
#endif
}

hu_error_t hu_skill_registry_uninstall(const char *name) {
    if (!name || !name[0])
        return HU_ERR_INVALID_ARGUMENT;

#if !defined(HU_IS_TEST) || !HU_IS_TEST
#ifdef HU_GATEWAY_POSIX
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return HU_ERR_INVALID_ARGUMENT;

    char dir_path[512];
    int n = snprintf(dir_path, sizeof(dir_path), "%s/.human/skills/%.256s", home, name);
    if (n <= 0 || (size_t)n >= sizeof(dir_path))
        return HU_ERR_INVALID_ARGUMENT;

    hu_error_t err = remove_dir_recursive(dir_path);
    if (err != HU_OK)
        return err;

    /* Also remove flat .skill.json for backward compatibility */
    char flat_path[512];
    n = snprintf(flat_path, sizeof(flat_path), "%s/.human/skills/%.256s.skill.json", home, name);
    if (n > 0 && (size_t)n < sizeof(flat_path))
        unlink(flat_path);

    return HU_OK;
#else
    (void)name;
    return HU_ERR_NOT_SUPPORTED;
#endif
#else
    (void)name;
    return HU_OK;
#endif
}

hu_error_t hu_skill_registry_update(hu_allocator_t *alloc, const char *source_path) {
    if (!alloc || !source_path || !source_path[0])
        return HU_ERR_INVALID_ARGUMENT;

#if !defined(HU_IS_TEST) || !HU_IS_TEST
    char manifest_path[512];
#ifdef HU_GATEWAY_POSIX
    if (!find_manifest_in_dir(source_path, manifest_path, sizeof(manifest_path)))
        return HU_ERR_NOT_FOUND;

    char buf[8192];
    FILE *f = fopen(manifest_path, "rb");
    if (!f)
        return HU_ERR_IO;
    size_t nr = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (nr == 0 || nr >= sizeof(buf) - 1)
        return HU_ERR_IO;
    buf[nr] = '\0';

    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, buf, nr, &parsed);
    if (err != HU_OK || !parsed || parsed->type != HU_JSON_OBJECT) {
        if (parsed)
            hu_json_free(alloc, parsed);
        return err != HU_OK ? err : HU_ERR_PARSE;
    }
    const char *name = hu_json_get_string(parsed, "name");
    if (!name || !name[0]) {
        hu_json_free(alloc, parsed);
        return HU_ERR_PARSE;
    }
    hu_json_free(alloc, parsed);

    hu_skill_registry_uninstall(name);
    return hu_skill_registry_install(alloc, source_path);
#else
    (void)alloc;
    (void)source_path;
    return HU_ERR_NOT_SUPPORTED;
#endif
#else
    (void)alloc;
    (void)source_path;
    return HU_OK;
#endif
}

hu_error_t hu_skill_registry_publish(hu_allocator_t *alloc, const char *skill_dir) {
    if (!alloc || !skill_dir)
        return HU_ERR_INVALID_ARGUMENT;

#if !defined(HU_IS_TEST) || !HU_IS_TEST
#ifdef HU_GATEWAY_POSIX
    char manifest_path[512];
    if (!find_manifest_in_dir(skill_dir, manifest_path, sizeof(manifest_path)))
        return HU_ERR_NOT_FOUND;

    char buf[8192];
    FILE *f = fopen(manifest_path, "rb");
    if (!f)
        return HU_ERR_IO;
    size_t read_len = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (read_len == 0 || read_len >= sizeof(buf))
        return HU_ERR_IO;
    buf[read_len] = '\0';

    hu_json_value_t *parsed = NULL;
    hu_error_t err = hu_json_parse(alloc, buf, read_len, &parsed);
    if (err != HU_OK || !parsed || parsed->type != HU_JSON_OBJECT) {
        if (parsed)
            hu_json_free(alloc, parsed);
        return err != HU_OK ? err : HU_ERR_PARSE;
    }

    const char *name = hu_json_get_string(parsed, "name");
    const char *desc = hu_json_get_string(parsed, "description");
    if (!name || !name[0] || !desc) {
        hu_json_free(alloc, parsed);
        return HU_ERR_PARSE;
    }
    hu_json_free(alloc, parsed);

    /* Write manifest to .published marker (local-only publish) */
    char published_path[512];
    int n = snprintf(published_path, sizeof(published_path), "%s/.published", skill_dir);
    if (n <= 0 || (size_t)n >= sizeof(published_path))
        return HU_ERR_INVALID_ARGUMENT;
    /* Local "published" marker for skills the user has published —
     * not secret, but path is user-derived; use the helper. */
    FILE *pf = NULL;
    if (hu_io_secure_open(published_path, HU_IO_PERM_USER, "wb", &pf) != HU_OK || !pf)
        return HU_ERR_IO;
    size_t written = fwrite(buf, 1, read_len, pf);
    fclose(pf);
    if (written != read_len)
        return HU_ERR_IO;

    return HU_OK;
#else
    (void)alloc;
    (void)skill_dir;
    return HU_ERR_NOT_SUPPORTED;
#endif
#else
    (void)alloc;
    (void)skill_dir;
    return HU_OK;
#endif
}

size_t hu_skill_registry_get_installed_dir(char *out, size_t out_len) {
    if (!out || out_len == 0)
        return 0;
    const char *home = getenv("HOME");
    if (!home) {
        out[0] = '\0';
        return 0;
    }
    int n = snprintf(out, out_len, "%s/.human/skills", home);
    return (n > 0 && (size_t)n < out_len) ? (size_t)n : 0;
}

#endif /* !HU_IS_TEST */
