#include "seaclaw/core/allocator.h"
#include "seaclaw/core/error.h"
#include "seaclaw/core/json.h"
#include "seaclaw/core/string.h"
#include "seaclaw/memory.h"
#include "seaclaw/tool.h"
#include <stdlib.h>
#include <string.h>

#define SC_MEMORY_STORE_NAME "memory_store"
#define SC_MEMORY_STORE_DESC "Store to memory"
#define SC_MEMORY_STORE_PARAMS                                                                  \
    "{\"type\":\"object\",\"properties\":{\"key\":{\"type\":\"string\"},\"content\":{\"type\":" \
    "\"string\"}},\"required\":[\"key\",\"content\"]}"
#define SC_MEMORY_KEY_MAX     1024
#define SC_MEMORY_CONTENT_MAX 256000

typedef struct sc_memory_store_ctx {
    sc_memory_t *memory;
} sc_memory_store_ctx_t;

static sc_error_t memory_store_execute(void *ctx, sc_allocator_t *alloc,
                                       const sc_json_value_t *args, sc_tool_result_t *out) {
    sc_memory_store_ctx_t *c = (sc_memory_store_ctx_t *)ctx;
    if (!c || !args || !out) {
        *out = sc_tool_result_fail("invalid args", 12);
        return SC_ERR_INVALID_ARGUMENT;
    }
    const char *key = sc_json_get_string(args, "key");
    const char *content = sc_json_get_string(args, "content");
    if (!key || strlen(key) == 0) {
        *out = sc_tool_result_fail("missing key", 11);
        return SC_OK;
    }
    if (strlen(key) > SC_MEMORY_KEY_MAX) {
        *out = sc_tool_result_fail("key too long", 12);
        return SC_OK;
    }
    if (!content)
        content = "";
    if (strlen(content) > SC_MEMORY_CONTENT_MAX) {
        *out = sc_tool_result_fail("content too long", 16);
        return SC_OK;
    }
#if SC_IS_TEST
    char *msg = sc_strndup(alloc, "(memory_store stub)", 19);
    if (!msg) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(msg, 19);
    return SC_OK;
#else
    if (!c->memory || !c->memory->vtable) {
        *out = sc_tool_result_fail("memory not configured", 21);
        return SC_OK;
    }
    sc_memory_category_t cat = {.tag = SC_MEMORY_CATEGORY_CUSTOM};
    const char *sid = c->memory->current_session_id;
    size_t sid_len = c->memory->current_session_id_len;
    sc_error_t err = c->memory->vtable->store(c->memory->ctx, key, strlen(key), content,
                                              strlen(content), &cat, sid, sid_len);
    if (err != SC_OK) {
        *out = sc_tool_result_fail("store failed", 12);
        return SC_OK;
    }
    char *ok = sc_strndup(alloc, "stored", 6);
    if (!ok) {
        *out = sc_tool_result_fail("out of memory", 12);
        return SC_ERR_OUT_OF_MEMORY;
    }
    *out = sc_tool_result_ok_owned(ok, 6);
    return SC_OK;
#endif
}

static const char *memory_store_name(void *ctx) {
    (void)ctx;
    return SC_MEMORY_STORE_NAME;
}
static const char *memory_store_description(void *ctx) {
    (void)ctx;
    return SC_MEMORY_STORE_DESC;
}
static const char *memory_store_parameters_json(void *ctx) {
    (void)ctx;
    return SC_MEMORY_STORE_PARAMS;
}
static void memory_store_deinit(void *ctx, sc_allocator_t *alloc) {
    if (ctx)
        alloc->free(alloc->ctx, ctx, sizeof(sc_memory_store_ctx_t));
}

static const sc_tool_vtable_t memory_store_vtable = {
    .execute = memory_store_execute,
    .name = memory_store_name,
    .description = memory_store_description,
    .parameters_json = memory_store_parameters_json,
    .deinit = memory_store_deinit,
};

sc_error_t sc_memory_store_create(sc_allocator_t *alloc, sc_memory_t *memory, sc_tool_t *out) {
    sc_memory_store_ctx_t *c = (sc_memory_store_ctx_t *)alloc->alloc(alloc->ctx, sizeof(*c));
    if (!c)
        return SC_ERR_OUT_OF_MEMORY;
    memset(c, 0, sizeof(*c));
    c->memory = memory;
    out->ctx = c;
    out->vtable = &memory_store_vtable;
    return SC_OK;
}
