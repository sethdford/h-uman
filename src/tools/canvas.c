#include "seaclaw/tools/canvas.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdio.h>

#define TOOL_NAME "canvas"
#define TOOL_DESC "Collaborative document canvas. Create, edit sections, and render documents."
#define TOOL_PARAMS \
    "{\"type\":\"object\",\"properties\":{" \
    "\"action\":{\"type\":\"string\",\"enum\":[\"create\",\"edit\",\"view\",\"delete\"]}," \
    "\"id\":{\"type\":\"string\"}," \
    "\"title\":{\"type\":\"string\"}," \
    "\"content\":{\"type\":\"string\"}," \
    "\"section\":{\"type\":\"integer\"}" \
    "},\"required\":[\"action\"]}"

#define CANVAS_MAX 16
#define CANVAS_MAX_CONTENT 16384

typedef struct {
    char *id;
    char *title;
    char *content;
} canvas_doc_t;

typedef struct {
    sc_allocator_t *alloc;
    canvas_doc_t docs[CANVAS_MAX];
    size_t count;
    uint32_t next_id;
} canvas_ctx_t;

static canvas_doc_t *canvas_find(canvas_ctx_t *c, const char *id) {
    for (size_t i = 0; i < c->count; i++)
        if (c->docs[i].id && strcmp(c->docs[i].id, id) == 0)
            return &c->docs[i];
    return NULL;
}

static sc_error_t canvas_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out)
{
    canvas_ctx_t *c = (canvas_ctx_t *)ctx;
    if (!args || !out) { *out = sc_tool_result_fail("invalid args", 12); return SC_OK; }

    const char *action = sc_json_get_string(args, "action");
    if (!action) { *out = sc_tool_result_fail("missing action", 14); return SC_OK; }

    if (strcmp(action, "create") == 0) {
        if (c->count >= CANVAS_MAX) {
            *out = sc_tool_result_fail("canvas limit reached", 20); return SC_OK;
        }
        const char *title = sc_json_get_string(args, "title");
        const char *content = sc_json_get_string(args, "content");
        char id_buf[16];
        int n = snprintf(id_buf, sizeof(id_buf), "doc_%u", c->next_id++);
        c->docs[c->count].id = sc_strndup(c->alloc, id_buf, (size_t)n);
        c->docs[c->count].title = title ? sc_strndup(c->alloc, title, strlen(title)) : NULL;
        c->docs[c->count].content = content ? sc_strndup(c->alloc, content, strlen(content)) : sc_strndup(c->alloc, "", 0);
        c->count++;
        char *msg = sc_sprintf(alloc, "{\"id\":\"%s\",\"created\":true}", id_buf);
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "edit") == 0) {
        const char *id = sc_json_get_string(args, "id");
        if (!id) { *out = sc_tool_result_fail("missing id", 10); return SC_OK; }
        canvas_doc_t *doc = canvas_find(c, id);
        if (!doc) { *out = sc_tool_result_fail("document not found", 18); return SC_OK; }
        const char *content = sc_json_get_string(args, "content");
        if (content) {
            if (doc->content) c->alloc->free(c->alloc->ctx, doc->content, strlen(doc->content) + 1);
            doc->content = sc_strndup(c->alloc, content, strlen(content));
        }
        const char *title = sc_json_get_string(args, "title");
        if (title) {
            if (doc->title) c->alloc->free(c->alloc->ctx, doc->title, strlen(doc->title) + 1);
            doc->title = sc_strndup(c->alloc, title, strlen(title));
        }
        *out = sc_tool_result_ok("updated", 7);
    } else if (strcmp(action, "view") == 0) {
        const char *id = sc_json_get_string(args, "id");
        if (!id) { *out = sc_tool_result_fail("missing id", 10); return SC_OK; }
        canvas_doc_t *doc = canvas_find(c, id);
        if (!doc) { *out = sc_tool_result_fail("document not found", 18); return SC_OK; }
        char *msg = sc_sprintf(alloc, "{\"id\":\"%s\",\"title\":\"%s\",\"content\":\"%s\"}",
            doc->id ? doc->id : "",
            doc->title ? doc->title : "",
            doc->content ? doc->content : "");
        *out = sc_tool_result_ok_owned(msg, msg ? strlen(msg) : 0);
    } else if (strcmp(action, "delete") == 0) {
        const char *id = sc_json_get_string(args, "id");
        if (!id) { *out = sc_tool_result_fail("missing id", 10); return SC_OK; }
        bool found = false;
        for (size_t i = 0; i < c->count; i++) {
            if (c->docs[i].id && strcmp(c->docs[i].id, id) == 0) {
                if (c->docs[i].id) c->alloc->free(c->alloc->ctx, c->docs[i].id, strlen(c->docs[i].id) + 1);
                if (c->docs[i].title) c->alloc->free(c->alloc->ctx, c->docs[i].title, strlen(c->docs[i].title) + 1);
                if (c->docs[i].content) c->alloc->free(c->alloc->ctx, c->docs[i].content, strlen(c->docs[i].content) + 1);
                if (i + 1 < c->count) c->docs[i] = c->docs[c->count - 1];
                memset(&c->docs[c->count - 1], 0, sizeof(canvas_doc_t));
                c->count--;
                found = true;
                break;
            }
        }
        *out = found ? sc_tool_result_ok("deleted", 7) : sc_tool_result_fail("not found", 9);
    } else {
        *out = sc_tool_result_fail("unknown action", 14);
    }
    return SC_OK;
}

static const char *canvas_name(void *ctx) { (void)ctx; return TOOL_NAME; }
static const char *canvas_desc(void *ctx) { (void)ctx; return TOOL_DESC; }
static const char *canvas_params(void *ctx) { (void)ctx; return TOOL_PARAMS; }
static void canvas_deinit(void *ctx, sc_allocator_t *alloc) {
    canvas_ctx_t *c = (canvas_ctx_t *)ctx;
    if (!c) return;
    for (size_t i = 0; i < c->count; i++) {
        if (c->docs[i].id) alloc->free(alloc->ctx, c->docs[i].id, strlen(c->docs[i].id) + 1);
        if (c->docs[i].title) alloc->free(alloc->ctx, c->docs[i].title, strlen(c->docs[i].title) + 1);
        if (c->docs[i].content) alloc->free(alloc->ctx, c->docs[i].content, strlen(c->docs[i].content) + 1);
    }
    alloc->free(alloc->ctx, c, sizeof(*c));
}

static const sc_tool_vtable_t canvas_vtable = {
    .execute = canvas_execute,
    .name = canvas_name,
    .description = canvas_desc,
    .parameters_json = canvas_params,
    .deinit = canvas_deinit,
};

sc_error_t sc_canvas_create(sc_allocator_t *alloc, sc_tool_t *out) {
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    canvas_ctx_t *c = (canvas_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    out->ctx = c;
    out->vtable = &canvas_vtable;
    return SC_OK;
}
