#include "seaclaw/tools/notebook.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include <string.h>
#include <stdio.h>

#define TOOL_NAME "notebook"
#define TOOL_DESC "Persistent scratchpad for working notes. Supports add, read, clear, and list."
#define TOOL_PARAMS \
    "{\"type\":\"object\",\"properties\":{" \
    "\"action\":{\"type\":\"string\",\"enum\":[\"add\",\"read\",\"clear\",\"list\"]}," \
    "\"key\":{\"type\":\"string\"}," \
    "\"content\":{\"type\":\"string\"}" \
    "},\"required\":[\"action\"]}"

#define NB_MAX_ENTRIES 64
#define NB_MAX_VALUE 4096

typedef struct {
    char *key;
    char *value;
} nb_entry_t;

typedef struct {
    sc_allocator_t *alloc;
    nb_entry_t entries[NB_MAX_ENTRIES];
    size_t count;
} notebook_ctx_t;

static nb_entry_t *nb_find(notebook_ctx_t *c, const char *key) {
    for (size_t i = 0; i < c->count; i++)
        if (c->entries[i].key && strcmp(c->entries[i].key, key) == 0)
            return &c->entries[i];
    return NULL;
}

static sc_error_t notebook_execute(void *ctx, sc_allocator_t *alloc,
    const sc_json_value_t *args, sc_tool_result_t *out)
{
    notebook_ctx_t *c = (notebook_ctx_t *)ctx;
    if (!args || !out) { *out = sc_tool_result_fail("invalid args", 12); return SC_OK; }

    const char *action = sc_json_get_string(args, "action");
    if (!action) { *out = sc_tool_result_fail("missing action", 14); return SC_OK; }

    if (strcmp(action, "add") == 0) {
        const char *key = sc_json_get_string(args, "key");
        const char *content = sc_json_get_string(args, "content");
        if (!key || !content) {
            *out = sc_tool_result_fail("missing key or content", 22); return SC_OK;
        }
        nb_entry_t *existing = nb_find(c, key);
        if (existing) {
            size_t vlen = strlen(existing->value);
            c->alloc->free(c->alloc->ctx, existing->value, vlen + 1);
            existing->value = sc_strndup(c->alloc, content, strlen(content));
        } else {
            if (c->count >= NB_MAX_ENTRIES) {
                *out = sc_tool_result_fail("notebook full", 13); return SC_OK;
            }
            c->entries[c->count].key = sc_strndup(c->alloc, key, strlen(key));
            c->entries[c->count].value = sc_strndup(c->alloc, content, strlen(content));
            c->count++;
        }
        *out = sc_tool_result_ok("stored", 6);
    } else if (strcmp(action, "read") == 0) {
        const char *key = sc_json_get_string(args, "key");
        if (!key) { *out = sc_tool_result_fail("missing key", 11); return SC_OK; }
        nb_entry_t *e = nb_find(c, key);
        if (!e) { *out = sc_tool_result_fail("not found", 9); return SC_OK; }
        *out = sc_tool_result_ok(e->value, strlen(e->value));
    } else if (strcmp(action, "clear") == 0) {
        for (size_t i = 0; i < c->count; i++) {
            if (c->entries[i].key) c->alloc->free(c->alloc->ctx, c->entries[i].key, strlen(c->entries[i].key) + 1);
            if (c->entries[i].value) c->alloc->free(c->alloc->ctx, c->entries[i].value, strlen(c->entries[i].value) + 1);
        }
        c->count = 0;
        *out = sc_tool_result_ok("cleared", 7);
    } else if (strcmp(action, "list") == 0) {
        size_t cap = 256;
        char *buf = (char *)alloc->alloc(alloc->ctx, cap);
        if (!buf) { *out = sc_tool_result_fail("oom", 3); return SC_OK; }
        size_t off = 0;
        off += (size_t)snprintf(buf + off, cap - off, "{\"keys\":[");
        for (size_t i = 0; i < c->count; i++) {
            if (i > 0 && off < cap) buf[off++] = ',';
            off += (size_t)snprintf(buf + off, cap - off, "\"%s\"", c->entries[i].key ? c->entries[i].key : "");
        }
        off += (size_t)snprintf(buf + off, cap - off, "]}");
        *out = sc_tool_result_ok_owned(buf, off);
    } else {
        *out = sc_tool_result_fail("unknown action", 14);
    }
    return SC_OK;
}

static const char *notebook_name(void *ctx) { (void)ctx; return TOOL_NAME; }
static const char *notebook_desc(void *ctx) { (void)ctx; return TOOL_DESC; }
static const char *notebook_params(void *ctx) { (void)ctx; return TOOL_PARAMS; }
static void notebook_deinit(void *ctx, sc_allocator_t *alloc) {
    notebook_ctx_t *c = (notebook_ctx_t *)ctx;
    if (!c) return;
    for (size_t i = 0; i < c->count; i++) {
        if (c->entries[i].key) alloc->free(alloc->ctx, c->entries[i].key, strlen(c->entries[i].key) + 1);
        if (c->entries[i].value) alloc->free(alloc->ctx, c->entries[i].value, strlen(c->entries[i].value) + 1);
    }
    alloc->free(alloc->ctx, c, sizeof(*c));
}

static const sc_tool_vtable_t notebook_vtable = {
    .execute = notebook_execute,
    .name = notebook_name,
    .description = notebook_desc,
    .parameters_json = notebook_params,
    .deinit = notebook_deinit,
};

sc_error_t sc_notebook_create(sc_allocator_t *alloc,
    sc_security_policy_t *policy, sc_tool_t *out)
{
    (void)policy;
    if (!alloc || !out) return SC_ERR_INVALID_ARGUMENT;
    notebook_ctx_t *c = (notebook_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c) return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->alloc = alloc;
    out->ctx = c;
    out->vtable = &notebook_vtable;
    return SC_OK;
}
