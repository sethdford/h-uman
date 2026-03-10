#include "human/tools/canvas.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/tools/validation.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define TOOL_NAME "canvas"
#define TOOL_DESC "Collaborative document canvas. Create, edit sections, and render documents."
#define TOOL_PARAMS                                                                                \
    "{\"type\":\"object\",\"properties\":{\"action\":{\"type\":\"string\",\"enum\":[\"create\","   \
    "\"edit\",\"view\",\"delete\"]},\"id\":{\"type\":\"string\"},\"title\":{\"type\":\"string\"}," \
    "\"content\":{\"type\":\"string\"},\"section\":{\"type\":\"integer\"}},\"required\":["         \
    "\"action\"]}"
#define CANVAS_MAX         16
#define CANVAS_MAX_CONTENT 16384
#define CANVAS_DIR_SUFFIX  "/.human/canvas"
typedef struct {
    char *id;
    char *title;
    char *content;
} canvas_doc_t;
typedef struct {
    hu_allocator_t *alloc;
    canvas_doc_t docs[CANVAS_MAX];
    size_t count;
    uint32_t next_id;
    char persist_dir[512];
} canvas_ctx_t;

static int canvas_id_is_safe(const char *id) {
    if (!id || id[0] == '\0')
        return 0;
    for (const char *p = id; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        if (ch == '/' || ch == '\\' || ch == '\0')
            return 0;
    }
    if (strstr(id, "..") != NULL)
        return 0;
    return 1;
}

static void canvas_persist_doc(canvas_ctx_t *c, canvas_doc_t *doc) {
    if (!c->persist_dir[0] || !doc->id)
        return;
    if (!canvas_id_is_safe(doc->id))
        return;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s.md", c->persist_dir, doc->id);
    if (n < 0 || (size_t)n >= sizeof(path))
        return;
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    if (doc->title)
        fprintf(f, "# %s\n\n", doc->title);
    if (doc->content)
        fputs(doc->content, f);
    fclose(f);
}
static void canvas_remove_file(canvas_ctx_t *c, const char *id) {
    if (!c->persist_dir[0] || !id || !canvas_id_is_safe(id))
        return;
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s.md", c->persist_dir, id);
    if (n < 0 || (size_t)n >= sizeof(path))
        return;
    remove(path);
}
static void canvas_save_index(canvas_ctx_t *c) {
    if (!c->persist_dir[0])
        return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/.index", c->persist_dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    for (size_t i = 0; i < c->count; i++)
        if (c->docs[i].id)
            fprintf(f, "%s\t%s\n", c->docs[i].id, c->docs[i].title ? c->docs[i].title : "");
    fclose(f);
}
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
static void canvas_init_persist_dir(canvas_ctx_t *c) {
    if (!c->persist_dir[0])
        return;
    struct stat st;
    if (stat(c->persist_dir, &st) != 0) {
        char tmp[512];
        snprintf(tmp, sizeof(tmp), "%s", c->persist_dir);
        for (char *p = tmp + 1; *p; p++) {
            if (*p == '/') {
                *p = '\0';
                (void)mkdir(tmp, 0700);
                *p = '/';
            }
        }
        (void)mkdir(tmp, 0700);
    }
}
static void canvas_load_from_disk(canvas_ctx_t *c) {
    if (!c->persist_dir[0])
        return;
    char idx_path[1024];
    snprintf(idx_path, sizeof(idx_path), "%s/.index", c->persist_dir);
    FILE *idx = fopen(idx_path, "r");
    if (!idx)
        return;
    char line[256];
    while (fgets(line, sizeof(line), idx) && c->count < CANVAS_MAX) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        if (len == 0)
            continue;
        char *tab = strchr(line, '\t');
        char *id_str = line;
        char *title_str = NULL;
        if (tab) {
            *tab = '\0';
            title_str = tab + 1;
        }
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s.md", c->persist_dir, id_str);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        fseek(f, 0, SEEK_END);
        long flen = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (flen < 0 || (size_t)flen > CANVAS_MAX_CONTENT) {
            fclose(f);
            continue;
        }
        char *buf = (char *)c->alloc->alloc(c->alloc->ctx, (size_t)flen + 1);
        if (!buf) {
            fclose(f);
            continue;
        }
        size_t rd = fread(buf, 1, (size_t)flen, f);
        buf[rd] = '\0';
        fclose(f);
        const char *content = buf;
        char *real_title = NULL;
        if (buf[0] == '#' && buf[1] == ' ') {
            char *nl = strchr(buf + 2, '\n');
            if (nl) {
                real_title = hu_strndup(c->alloc, buf + 2, (size_t)(nl - (buf + 2)));
                content = nl + 1;
                while (*content == '\n')
                    content++;
            }
        }
        if (!real_title && title_str)
            real_title = hu_strndup(c->alloc, title_str, strlen(title_str));
        unsigned id_num = 0;
        if (sscanf(id_str, "doc_%u", &id_num) == 1 && id_num >= c->next_id)
            c->next_id = id_num + 1;
        c->docs[c->count].id = hu_strndup(c->alloc, id_str, strlen(id_str));
        c->docs[c->count].title = real_title;
        c->docs[c->count].content = hu_strndup(c->alloc, content, strlen(content));
        c->alloc->free(c->alloc->ctx, buf, (size_t)flen + 1);
        c->count++;
    }
    fclose(idx);
}
#endif
static canvas_doc_t *canvas_find(canvas_ctx_t *c, const char *id) {
    for (size_t i = 0; i < c->count; i++)
        if (c->docs[i].id && strcmp(c->docs[i].id, id) == 0)
            return &c->docs[i];
    return NULL;
}

static hu_error_t canvas_execute(void *ctx, hu_allocator_t *alloc, const hu_json_value_t *args,
                                 hu_tool_result_t *out) {
    canvas_ctx_t *c = (canvas_ctx_t *)ctx;
    if (!out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!args) {
        *out = hu_tool_result_fail("invalid args", 12);
        return HU_ERR_INVALID_ARGUMENT;
    }
    const char *action = hu_json_get_string(args, "action");
    if (!action) {
        *out = hu_tool_result_fail("missing action", 14);
        return HU_OK;
    }
    if (strcmp(action, "create") == 0) {
        if (c->count >= CANVAS_MAX) {
            *out = hu_tool_result_fail("canvas limit reached", 20);
            return HU_OK;
        }
        const char *title = hu_json_get_string(args, "title");
        const char *content = hu_json_get_string(args, "content");
        char id_buf[16];
        int n = snprintf(id_buf, sizeof(id_buf), "doc_%u", c->next_id++);
        if (n < 0)
            n = 0;
        c->docs[c->count].id = hu_strndup(c->alloc, id_buf, (size_t)n);
        c->docs[c->count].title = title ? hu_strndup(c->alloc, title, strlen(title)) : NULL;
        c->docs[c->count].content =
            content ? hu_strndup(c->alloc, content, strlen(content)) : hu_strndup(c->alloc, "", 0);
        c->count++;
        canvas_persist_doc(c, &c->docs[c->count - 1]);
        canvas_save_index(c);
        char *msg = hu_sprintf(alloc, "{\"id\":\"%s\",\"created\":true}", id_buf);
        *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "edit") == 0) {
        const char *id = hu_json_get_string(args, "id");
        if (!id) {
            *out = hu_tool_result_fail("missing id", 10);
            return HU_OK;
        }
        canvas_doc_t *doc = canvas_find(c, id);
        if (!doc) {
            *out = hu_tool_result_fail("document not found", 18);
            return HU_OK;
        }
        const char *content = hu_json_get_string(args, "content");
        if (content) {
            if (doc->content)
                c->alloc->free(c->alloc->ctx, doc->content, strlen(doc->content) + 1);
            doc->content = hu_strndup(c->alloc, content, strlen(content));
        }
        const char *title = hu_json_get_string(args, "title");
        if (title) {
            if (doc->title)
                c->alloc->free(c->alloc->ctx, doc->title, strlen(doc->title) + 1);
            doc->title = hu_strndup(c->alloc, title, strlen(title));
        }
        canvas_persist_doc(c, doc);
        canvas_save_index(c);
        *out = hu_tool_result_ok("updated", 7);
    } else if (strcmp(action, "view") == 0) {
        const char *id = hu_json_get_string(args, "id");
        if (!id) {
            *out = hu_tool_result_fail("missing id", 10);
            return HU_OK;
        }
        canvas_doc_t *doc = canvas_find(c, id);
        if (!doc) {
            *out = hu_tool_result_fail("document not found", 18);
            return HU_OK;
        }
        char *msg = hu_sprintf(alloc, "{\"id\":\"%s\",\"title\":\"%s\",\"content\":\"%s\"}",
                               doc->id ? doc->id : "", doc->title ? doc->title : "",
                               doc->content ? doc->content : "");
        *out = hu_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "delete") == 0) {
        const char *id = hu_json_get_string(args, "id");
        if (!id) {
            *out = hu_tool_result_fail("missing id", 10);
            return HU_OK;
        }
        bool found = false;
        for (size_t i = 0; i < c->count; i++) {
            if (c->docs[i].id && strcmp(c->docs[i].id, id) == 0) {
                if (c->docs[i].id)
                    c->alloc->free(c->alloc->ctx, c->docs[i].id, strlen(c->docs[i].id) + 1);
                if (c->docs[i].title)
                    c->alloc->free(c->alloc->ctx, c->docs[i].title, strlen(c->docs[i].title) + 1);
                if (c->docs[i].content)
                    c->alloc->free(c->alloc->ctx, c->docs[i].content,
                                   strlen(c->docs[i].content) + 1);
                if (i + 1 < c->count)
                    c->docs[i] = c->docs[c->count - 1];
                memset(&c->docs[c->count - 1], 0, sizeof(canvas_doc_t));
                c->count--;
                found = true;
                canvas_remove_file(c, id);
                canvas_save_index(c);
                break;
            }
        }
        *out = found ? hu_tool_result_ok("deleted", 7) : hu_tool_result_fail("not found", 9);
    } else {
        *out = hu_tool_result_fail("unknown action", 14);
    }
    return HU_OK;
}
static const char *canvas_name(void *ctx) {
    (void)ctx;
    return TOOL_NAME;
}
static const char *canvas_desc(void *ctx) {
    (void)ctx;
    return TOOL_DESC;
}
static const char *canvas_params(void *ctx) {
    (void)ctx;
    return TOOL_PARAMS;
}
static void canvas_deinit(void *ctx, hu_allocator_t *alloc) {
    canvas_ctx_t *c = (canvas_ctx_t *)ctx;
    if (!c)
        return;
    for (size_t i = 0; i < c->count; i++) {
        if (c->docs[i].id)
            alloc->free(alloc->ctx, c->docs[i].id, strlen(c->docs[i].id) + 1);
        if (c->docs[i].title)
            alloc->free(alloc->ctx, c->docs[i].title, strlen(c->docs[i].title) + 1);
        if (c->docs[i].content)
            alloc->free(alloc->ctx, c->docs[i].content, strlen(c->docs[i].content) + 1);
    }
    alloc->free(alloc->ctx, c, sizeof(*c));
}
static const hu_tool_vtable_t canvas_vtable = {.execute = canvas_execute,
                                               .name = canvas_name,
                                               .description = canvas_desc,
                                               .parameters_json = canvas_params,
                                               .deinit = canvas_deinit};

hu_error_t hu_canvas_create(hu_allocator_t *alloc, hu_tool_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    canvas_ctx_t *c = (canvas_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return HU_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
#if !defined(HU_IS_TEST) || HU_IS_TEST == 0
    const char *home = getenv("HOME");
    if (home) {
        snprintf(c->persist_dir, sizeof(c->persist_dir), "%s%s", home, CANVAS_DIR_SUFFIX);
        canvas_init_persist_dir(c);
        canvas_load_from_disk(c);
    }
#endif
    out->ctx = c;
    out->vtable = &canvas_vtable;
    return HU_OK;
}
