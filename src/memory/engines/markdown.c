#include "seaclaw/memory.h"
#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define sc_mkdir(path) _mkdir(path)
#else
#include <unistd.h>
#include <dirent.h>
#define sc_mkdir(path) mkdir((path), 0755)
#endif

typedef struct sc_markdown_memory {
    char *dir;
    size_t dir_len;
    sc_allocator_t *alloc;
} sc_markdown_memory_t;

static const char *impl_name(void *ctx) {
    (void)ctx;
    return "markdown";
}

static const char *category_to_string(const sc_memory_category_t *cat) {
    if (!cat) return "core";
    switch (cat->tag) {
        case SC_MEMORY_CATEGORY_CORE: return "core";
        case SC_MEMORY_CATEGORY_DAILY: return "daily";
        case SC_MEMORY_CATEGORY_CONVERSATION: return "conversation";
        case SC_MEMORY_CATEGORY_CUSTOM:
            if (cat->data.custom.name && cat->data.custom.name_len > 0)
                return cat->data.custom.name;
            return "custom";
        default: return "core";
    }
}

static char *key_to_filename(const char *key, size_t key_len, sc_allocator_t *alloc) {
    /* Sanitize key for filename: replace invalid chars with _ */
    char *out = (char *)alloc->alloc(alloc->ctx, key_len + 8);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < key_len && j < key_len + 4; i++) {
        char c = key[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.')
            out[j++] = c;
        else
            out[j++] = '_';
    }
    memcpy(out + j, ".md", 4);
    return out;
}

static void ensure_dir(const char *path) {
    char tmp[1024];
    size_t len = strlen(path);
    if (len >= sizeof(tmp)) return;
    memcpy(tmp, path, len + 1);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char save = *p;
            *p = '\0';
            sc_mkdir(tmp);
            *p = save;
        }
    }
    sc_mkdir(tmp);
}

static sc_error_t impl_store(void *ctx,
    const char *key, size_t key_len,
    const char *content, size_t content_len,
    const sc_memory_category_t *category,
    const char *session_id, size_t session_id_len) {
    sc_markdown_memory_t *self = (sc_markdown_memory_t *)ctx;
    const char *cat_str = category_to_string(category);

    char *filename = key_to_filename(key, key_len, self->alloc);
    if (!filename) return SC_ERR_OUT_OF_MEMORY;

    char *fullpath = sc_sprintf(self->alloc, "%s/%s", self->dir, filename);
    self->alloc->free(self->alloc->ctx, filename, strlen(filename) + 1);
    if (!fullpath) return SC_ERR_OUT_OF_MEMORY;

    ensure_dir(self->dir);

    time_t t = time(NULL);
    char ts[32];
    snprintf(ts, sizeof(ts), "%ld", (long)t);

    /* YAML-like frontmatter */
    size_t header_len = 128 + key_len + strlen(cat_str) + 32;
    char *header = (char *)self->alloc->alloc(self->alloc->ctx, header_len);
    if (!header) {
        self->alloc->free(self->alloc->ctx, fullpath, strlen(fullpath) + 1);
        return SC_ERR_OUT_OF_MEMORY;
    }
    int n = snprintf(header, header_len, "---\nkey: %.*s\ncategory: %s\ntimestamp: %s\n",
        (int)key_len, key, cat_str, ts);
    if (session_id && session_id_len > 0)
        n += snprintf(header + n, header_len - (size_t)n, "session_id: %.*s\n",
            (int)session_id_len, session_id);
    n += snprintf(header + n, header_len - (size_t)n, "---\n\n");

    FILE *f = fopen(fullpath, "w");
    self->alloc->free(self->alloc->ctx, header, header_len);
    self->alloc->free(self->alloc->ctx, fullpath, strlen(fullpath) + 1);
    if (!f) return SC_ERR_MEMORY_STORE;

    fprintf(f, "---\nkey: %.*s\ncategory: %s\ntimestamp: %s\n",
        (int)key_len, key, cat_str, ts);
    if (session_id && session_id_len > 0)
        fprintf(f, "session_id: %.*s\n", (int)session_id_len, session_id);
    fprintf(f, "---\n\n%.*s", (int)content_len, content);
    fclose(f);
    return SC_OK;
}

static sc_error_t impl_recall(void *ctx, sc_allocator_t *alloc,
    const char *query, size_t query_len,
    size_t limit,
    const char *session_id, size_t session_id_len,
    sc_memory_entry_t **out, size_t *out_count) {
    sc_markdown_memory_t *self = (sc_markdown_memory_t *)ctx;
    *out = NULL;
    *out_count = 0;

    sc_memory_entry_t *entries = (sc_memory_entry_t *)alloc->alloc(alloc->ctx,
        limit * sizeof(sc_memory_entry_t));
    if (!entries) return SC_ERR_OUT_OF_MEMORY;

    size_t count = 0;
#ifndef _WIN32
    DIR *d = opendir(self->dir);
    if (!d) {
        alloc->free(alloc->ctx, entries, limit * sizeof(sc_memory_entry_t));
        return SC_OK;
    }

    struct dirent *e;
    while (count < limit && (e = readdir(d)) != NULL) {
        size_t nlen = strlen(e->d_name);
        if (nlen < 4 || strcmp(e->d_name + nlen - 3, ".md") != 0) continue;

        char *path = sc_sprintf(self->alloc, "%s/%s", self->dir, e->d_name);
        if (!path) continue;

        FILE *f = fopen(path, "r");
        self->alloc->free(self->alloc->ctx, path, strlen(path) + 1);
        if (!f) continue;

        char line[4096];
        char key[256] = {0};
        char category[64] = "core";
        char timestamp[64] = {0};
        char sess[256] = {0};
        bool in_front = false;
        bool front_done = false;
        char content_buf[8192];
        size_t content_len = 0;

        while (fgets(line, sizeof(line), f) && content_len < sizeof(content_buf) - 1) {
            if (strncmp(line, "---", 3) == 0) {
                if (!in_front) in_front = true;
                else { front_done = true; continue; }
                continue;
            }
            if (!front_done) {
                if (strncmp(line, "key:", 4) == 0) {
                    const char *v = line + 4;
                    while (*v == ' ') v++;
                    size_t kv = 0;
                    while (*v && *v != '\n' && kv < sizeof(key) - 1) key[kv++] = *v++;
                    key[kv] = '\0';
                } else if (strncmp(line, "category:", 9) == 0) {
                    const char *v = line + 9;
                    while (*v == ' ') v++;
                    size_t cv = 0;
                    while (*v && *v != '\n' && cv < sizeof(category) - 1) category[cv++] = *v++;
                    category[cv] = '\0';
                } else if (strncmp(line, "timestamp:", 10) == 0) {
                    const char *v = line + 10;
                    while (*v == ' ') v++;
                    size_t tv = 0;
                    while (*v && *v != '\n' && tv < sizeof(timestamp) - 1) timestamp[tv++] = *v++;
                    timestamp[tv] = '\0';
                } else if (strncmp(line, "session_id:", 11) == 0) {
                    const char *v = line + 11;
                    while (*v == ' ') v++;
                    size_t sv = 0;
                    while (*v && *v != '\n' && sv < sizeof(sess) - 1) sess[sv++] = *v++;
                    sess[sv] = '\0';
                }
            } else {
                size_t linelen = strlen(line);
                if (content_len + linelen < sizeof(content_buf)) {
                    memcpy(content_buf + content_len, line, linelen + 1);
                    content_len += linelen;
                }
            }
        }
        fclose(f);

        if (session_id && session_id_len > 0 && sess[0] &&
            (strlen(sess) != session_id_len || memcmp(sess, session_id, session_id_len) != 0))
            continue;

        /* Simple substring search (query may not be null-terminated) */
        bool contains = false;
        if (query_len > 0) {
            for (size_t i = 0; i + query_len <= content_len && !contains; i++)
                if (memcmp(content_buf + i, query, query_len) == 0) contains = true;
            if (!contains && strlen(key) >= query_len)
                for (size_t i = 0; i + query_len <= strlen(key) && !contains; i++)
                    if (memcmp(key + i, query, query_len) == 0) contains = true;
        } else {
            contains = true;
        }
        if (!contains) continue;

        entries[count].id = sc_strdup(alloc, path ? path : e->d_name);
        entries[count].id_len = strlen(entries[count].id);
        entries[count].key = sc_strdup(alloc, key);
        entries[count].key_len = strlen(key);
        entries[count].content = sc_strndup(alloc, content_buf, content_len);
        entries[count].content_len = content_len;
        entries[count].category.tag = SC_MEMORY_CATEGORY_CUSTOM;
        entries[count].category.data.custom.name = sc_strdup(alloc, category);
        entries[count].category.data.custom.name_len = strlen(category);
        entries[count].timestamp = sc_strdup(alloc, timestamp);
        entries[count].timestamp_len = strlen(timestamp);
        entries[count].session_id = sess[0] ? sc_strdup(alloc, sess) : NULL;
        entries[count].session_id_len = sess[0] ? strlen(sess) : 0;
        entries[count].score = NAN;
        count++;
    }
    closedir(d);
#else
    (void)session_id;
    (void)session_id_len;
    (void)query;
    (void)query_len;
    alloc->free(alloc->ctx, entries, limit * sizeof(sc_memory_entry_t));
    return SC_OK;
#endif

    *out = entries;
    *out_count = count;
    return SC_OK;
}

static sc_error_t impl_get(void *ctx, sc_allocator_t *alloc,
    const char *key, size_t key_len,
    sc_memory_entry_t *out, bool *found) {
    sc_markdown_memory_t *self = (sc_markdown_memory_t *)ctx;
    *found = false;

    char *filename = key_to_filename(key, key_len, self->alloc);
    if (!filename) return SC_ERR_OUT_OF_MEMORY;
    char *fullpath = sc_sprintf(self->alloc, "%s/%s", self->dir, filename);
    self->alloc->free(self->alloc->ctx, filename, strlen(filename) + 1);
    if (!fullpath) return SC_ERR_OUT_OF_MEMORY;

    FILE *f = fopen(fullpath, "r");
    self->alloc->free(self->alloc->ctx, fullpath, strlen(fullpath) + 1);
    if (!f) return SC_OK;

    char line[4096];
    char category[64] = "core";
    char timestamp[64] = {0};
    bool front_done = false;
    char content_buf[8192];
    size_t content_len = 0;

    while (fgets(line, sizeof(line), f) && content_len < sizeof(content_buf) - 1) {
        if (strncmp(line, "---", 3) == 0) {
            if (front_done) continue;
            front_done = true;
            continue;
        }
        if (!front_done) {
            if (strncmp(line, "category:", 9) == 0) {
                const char *v = line + 9;
                while (*v == ' ') v++;
                size_t cv = 0;
                while (*v && *v != '\n' && cv < sizeof(category) - 1) category[cv++] = *v++;
                category[cv] = '\0';
            } else if (strncmp(line, "timestamp:", 10) == 0) {
                const char *v = line + 10;
                while (*v == ' ') v++;
                size_t tv = 0;
                while (*v && *v != '\n' && tv < sizeof(timestamp) - 1) timestamp[tv++] = *v++;
                timestamp[tv] = '\0';
            }
        } else {
            size_t linelen = strlen(line);
            if (content_len + linelen < sizeof(content_buf)) {
                memcpy(content_buf + content_len, line, linelen + 1);
                content_len += linelen;
            }
        }
    }
    fclose(f);

    out->id = sc_strndup(alloc, key, key_len);
    out->id_len = key_len;
    out->key = sc_strndup(alloc, key, key_len);
    out->key_len = key_len;
    out->content = sc_strndup(alloc, content_buf, content_len);
    out->content_len = content_len;
    out->category.tag = SC_MEMORY_CATEGORY_CUSTOM;
    out->category.data.custom.name = sc_strdup(alloc, category);
    out->category.data.custom.name_len = strlen(category);
    out->timestamp = sc_strdup(alloc, timestamp);
    out->timestamp_len = strlen(timestamp);
    out->session_id = NULL;
    out->session_id_len = 0;
    out->score = NAN;
    *found = true;
    return SC_OK;
}

static sc_error_t impl_list(void *ctx, sc_allocator_t *alloc,
    const sc_memory_category_t *category,
    const char *session_id, size_t session_id_len,
    sc_memory_entry_t **out, size_t *out_count) {
    (void)category;
    return impl_recall(ctx, alloc, "", 0, 1024, session_id, session_id_len, out, out_count);
}

static sc_error_t impl_forget(void *ctx,
    const char *key, size_t key_len,
    bool *deleted) {
    sc_markdown_memory_t *self = (sc_markdown_memory_t *)ctx;
    *deleted = false;
    char *filename = key_to_filename(key, key_len, self->alloc);
    if (!filename) return SC_OK;
    char *fullpath = sc_sprintf(self->alloc, "%s/%s", self->dir, filename);
    self->alloc->free(self->alloc->ctx, filename, strlen(filename) + 1);
    if (!fullpath) return SC_OK;
    if (remove(fullpath) == 0) *deleted = true;
    self->alloc->free(self->alloc->ctx, fullpath, strlen(fullpath) + 1);
    return SC_OK;
}

static sc_error_t impl_count(void *ctx, size_t *out) {
    sc_markdown_memory_t *self = (sc_markdown_memory_t *)ctx;
    *out = 0;
#ifndef _WIN32
    DIR *d = opendir(self->dir);
    if (!d) return SC_OK;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t nlen = strlen(e->d_name);
        if (nlen >= 4 && strcmp(e->d_name + nlen - 3, ".md") == 0) (*out)++;
    }
    closedir(d);
#endif
    return SC_OK;
}

static bool impl_health_check(void *ctx) {
    sc_markdown_memory_t *self = (sc_markdown_memory_t *)ctx;
    struct stat st;
    return stat(self->dir, &st) == 0 && S_ISDIR(st.st_mode);
}

static void impl_deinit(void *ctx) {
    sc_markdown_memory_t *self = (sc_markdown_memory_t *)ctx;
    self->alloc->free(self->alloc->ctx, self->dir, self->dir_len + 1);
    self->alloc->free(self->alloc->ctx, self, sizeof(sc_markdown_memory_t));
}

static const sc_memory_vtable_t markdown_vtable = {
    .name = impl_name,
    .store = impl_store,
    .recall = impl_recall,
    .get = impl_get,
    .list = impl_list,
    .forget = impl_forget,
    .count = impl_count,
    .health_check = impl_health_check,
    .deinit = impl_deinit,
};

sc_memory_t sc_markdown_memory_create(sc_allocator_t *alloc, const char *dir_path) {
    if (!alloc || !dir_path) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    size_t len = strlen(dir_path);
    char *dir = sc_strndup(alloc, dir_path, len);
    if (!dir) return (sc_memory_t){ .ctx = NULL, .vtable = NULL };

    sc_markdown_memory_t *self = (sc_markdown_memory_t *)alloc->alloc(alloc->ctx, sizeof(sc_markdown_memory_t));
    if (!self) {
        alloc->free(alloc->ctx, dir, len + 1);
        return (sc_memory_t){ .ctx = NULL, .vtable = NULL };
    }
    self->dir = dir;
    self->dir_len = len;
    self->alloc = alloc;
    return (sc_memory_t){
        .ctx = self,
        .vtable = &markdown_vtable,
    };
}
